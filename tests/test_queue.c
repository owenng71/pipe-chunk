#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include "queue.h"

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned ready;
    unsigned done;
    bool released;
} cohort_t;

static void cohort_init(cohort_t *cohort)
{
    assert(pthread_mutex_init(&cohort->mutex, NULL) == 0);
    assert(pthread_cond_init(&cohort->cond, NULL) == 0);
    cohort->ready = 0;
    cohort->done = 0;
    cohort->released = false;
}

static void cohort_destroy(cohort_t *cohort)
{
    assert(pthread_cond_destroy(&cohort->cond) == 0);
    assert(pthread_mutex_destroy(&cohort->mutex) == 0);
}

static void cohort_wait_for_release(cohort_t *cohort)
{
    assert(pthread_mutex_lock(&cohort->mutex) == 0);
    cohort->ready++;
    assert(pthread_cond_broadcast(&cohort->cond) == 0);
    while (!cohort->released)
        assert(pthread_cond_wait(&cohort->cond, &cohort->mutex) == 0);
    assert(pthread_mutex_unlock(&cohort->mutex) == 0);
}

static void cohort_release(cohort_t *cohort, unsigned expected_ready)
{
    assert(pthread_mutex_lock(&cohort->mutex) == 0);
    while (cohort->ready < expected_ready)
        assert(pthread_cond_wait(&cohort->cond, &cohort->mutex) == 0);
    cohort->released = true;
    assert(pthread_cond_broadcast(&cohort->cond) == 0);
    assert(pthread_mutex_unlock(&cohort->mutex) == 0);
}

static void cohort_mark_done(cohort_t *cohort)
{
    assert(pthread_mutex_lock(&cohort->mutex) == 0);
    cohort->done++;
    assert(pthread_cond_broadcast(&cohort->cond) == 0);
    assert(pthread_mutex_unlock(&cohort->mutex) == 0);
}

static unsigned cohort_done_count(cohort_t *cohort)
{
    unsigned done;

    assert(pthread_mutex_lock(&cohort->mutex) == 0);
    done = cohort->done;
    assert(pthread_mutex_unlock(&cohort->mutex) == 0);
    return done;
}

static struct timespec deadline_after_ms(long milliseconds)
{
    struct timespec deadline;

    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static void join_with_timeout(pthread_t thread, long timeout_ms)
{
    struct timespec deadline = deadline_after_ms(timeout_ms);

    assert(pthread_timedjoin_np(thread, NULL, &deadline) == 0);
}

static void cohort_assert_blocked_for(cohort_t *cohort, long milliseconds)
{
    struct timespec deadline = deadline_after_ms(milliseconds);
    int rc = 0;

    assert(pthread_mutex_lock(&cohort->mutex) == 0);
    while (cohort->done == 0 && rc == 0)
        rc = pthread_cond_timedwait(&cohort->cond, &cohort->mutex, &deadline);
    assert(cohort->done == 0);
    assert(rc == ETIMEDOUT);
    assert(pthread_mutex_unlock(&cohort->mutex) == 0);
}

static void cohort_wait_for_done(cohort_t *cohort, unsigned expected_done,
                                 long timeout_ms)
{
    struct timespec deadline = deadline_after_ms(timeout_ms);
    int rc = 0;

    assert(pthread_mutex_lock(&cohort->mutex) == 0);
    while (cohort->done < expected_done && rc == 0)
        rc = pthread_cond_timedwait(&cohort->cond, &cohort->mutex, &deadline);
    assert(cohort->done >= expected_done);
    assert(pthread_mutex_unlock(&cohort->mutex) == 0);
}

typedef struct {
    bq_t *queue;
    cohort_t *cohort;
    record_t *record;
    int result;
    int errno_after;
    bool caller_kept_record;
} producer_arg_t;

static void *blocked_producer(void *opaque)
{
    producer_arg_t *arg = opaque;

    cohort_wait_for_release(arg->cohort);
    errno = EACCES;
    arg->result = bq_push(arg->queue, arg->record);
    arg->errno_after = errno;
    arg->caller_kept_record = (arg->result == -1);
    cohort_mark_done(arg->cohort);
    return NULL;
}

typedef struct {
    bq_t *queue;
    cohort_t *cohort;
    int result;
    int errno_after;
    record_t *out;
} consumer_arg_t;

static void *blocked_consumer(void *opaque)
{
    consumer_arg_t *arg = opaque;

    cohort_wait_for_release(arg->cohort);
    errno = EACCES;
    arg->result = bq_pop(arg->queue, &arg->out);
    arg->errno_after = errno;
    cohort_mark_done(arg->cohort);
    return NULL;
}

typedef struct {
    bq_t *queue;
    cohort_t *cohort;
    int errno_after;
} closer_arg_t;

static void *concurrent_closer(void *opaque)
{
    closer_arg_t *arg = opaque;

    cohort_wait_for_release(arg->cohort);
    errno = EACCES;
    bq_close(arg->queue);
    arg->errno_after = errno;
    cohort_mark_done(arg->cohort);
    return NULL;
}

static void close_from_multiple_threads(bq_t *queue)
{
    enum { CLOSER_COUNT = 2 };
    closer_arg_t args[CLOSER_COUNT];
    pthread_t threads[CLOSER_COUNT];
    cohort_t cohort;

    cohort_init(&cohort);
    for (unsigned i = 0; i < CLOSER_COUNT; i++) {
        args[i].queue = queue;
        args[i].cohort = &cohort;
        args[i].errno_after = 0;
        assert(pthread_create(&threads[i], NULL, concurrent_closer, &args[i]) == 0);
    }
    cohort_release(&cohort, CLOSER_COUNT);
    for (unsigned i = 0; i < CLOSER_COUNT; i++) {
        join_with_timeout(threads[i], 5000);
        assert(args[i].errno_after == EACCES);
    }
    assert(cohort_done_count(&cohort) == CLOSER_COUNT);
    cohort_destroy(&cohort);
}

static void test_blocked_producers_resume_and_close(void)
{
    enum { WAITER_COUNT = 4 };
    record_t seed = { .seq = 100 };
    record_t produced[WAITER_COUNT] = {
        { .seq = 101 }, { .seq = 102 }, { .seq = 103 }, { .seq = 104 }
    };
    producer_arg_t args[WAITER_COUNT];
    pthread_t threads[WAITER_COUNT];
    cohort_t cohort;
    record_t *out = NULL;
    record_t *accepted_record = NULL;
    unsigned accepted = 0;
    bq_t *queue = bq_create(1);

    assert(queue != NULL);
    assert(bq_push(queue, &seed) == 0);
    cohort_init(&cohort);
    for (unsigned i = 0; i < WAITER_COUNT; i++) {
        args[i].queue = queue;
        args[i].cohort = &cohort;
        args[i].record = &produced[i];
        args[i].result = -2;
        args[i].errno_after = 0;
        args[i].caller_kept_record = false;
        assert(pthread_create(&threads[i], NULL, blocked_producer, &args[i]) == 0);
    }
    cohort_release(&cohort, WAITER_COUNT);
    cohort_assert_blocked_for(&cohort, 1000);

    errno = ERANGE;
    assert(bq_pop(queue, &out) == 1);
    assert(out == &seed);
    assert(errno == ERANGE);
    cohort_wait_for_done(&cohort, 1, 5000);
    assert(cohort_done_count(&cohort) == 1);

    close_from_multiple_threads(queue);
    for (unsigned i = 0; i < WAITER_COUNT; i++) {
        join_with_timeout(threads[i], 5000);
        assert(args[i].errno_after == EACCES);
        if (args[i].result == 0) {
            accepted++;
            accepted_record = args[i].record;
            assert(!args[i].caller_kept_record);
        } else {
            assert(args[i].result == -1);
            assert(args[i].caller_kept_record);
            assert(args[i].record->seq == 101 + i);
        }
    }
    assert(accepted == 1);
    assert(cohort_done_count(&cohort) == WAITER_COUNT);

    assert(bq_pop(queue, &out) == 1);
    assert(out == accepted_record);
    assert(bq_pop(queue, &out) == 0);
    assert(out == NULL);
    bq_close(queue);
    cohort_destroy(&cohort);
    bq_destroy(queue);
}

static void test_blocked_consumers_resume_and_close(void)
{
    enum { WAITER_COUNT = 4 };
    record_t item = { .seq = 200 };
    consumer_arg_t args[WAITER_COUNT];
    pthread_t threads[WAITER_COUNT];
    cohort_t cohort;
    bq_t *queue = bq_create(1);

    assert(queue != NULL);
    cohort_init(&cohort);
    for (unsigned i = 0; i < WAITER_COUNT; i++) {
        args[i].queue = queue;
        args[i].cohort = &cohort;
        args[i].result = -2;
        args[i].errno_after = 0;
        args[i].out = NULL;
        assert(pthread_create(&threads[i], NULL, blocked_consumer, &args[i]) == 0);
    }
    cohort_release(&cohort, WAITER_COUNT);
    cohort_assert_blocked_for(&cohort, 1000);

    errno = ERANGE;
    assert(bq_push(queue, &item) == 0);
    assert(errno == ERANGE);
    cohort_wait_for_done(&cohort, 1, 5000);
    assert(cohort_done_count(&cohort) == 1);

    close_from_multiple_threads(queue);
    unsigned received = 0;
    for (unsigned i = 0; i < WAITER_COUNT; i++) {
        join_with_timeout(threads[i], 5000);
        assert(args[i].errno_after == EACCES);
        if (args[i].result == 1) {
            received++;
            assert(args[i].out == &item);
        } else {
            assert(args[i].result == 0);
            assert(args[i].out == NULL);
        }
    }
    assert(received == 1);
    assert(cohort_done_count(&cohort) == WAITER_COUNT);
    assert(bq_size(queue) == 0);
    bq_close(queue);
    cohort_destroy(&cohort);
    bq_destroy(queue);
}

enum {
    PRODUCER_COUNT = 4,
    CONSUMER_COUNT = 4,
    ITEMS_PER_PRODUCER = 250,
    TOTAL_ITEMS = PRODUCER_COUNT * ITEMS_PER_PRODUCER,
    MPMC_CAPACITY = 16
};

typedef struct {
    bq_t *queue;
    record_t *records;
    unsigned producer_id;
    atomic_uint *accepted;
} mpmc_producer_arg_t;

typedef struct {
    bq_t *queue;
    atomic_uint *seen;
    atomic_uint *observed;
} mpmc_consumer_arg_t;

typedef struct {
    bq_t *queue;
    atomic_bool stop;
    atomic_bool exceeded_capacity;
    atomic_uint samples;
} monitor_arg_t;

static void *mpmc_producer(void *opaque)
{
    mpmc_producer_arg_t *arg = opaque;

    for (unsigned i = 0; i < ITEMS_PER_PRODUCER; i++) {
        unsigned index = arg->producer_id * ITEMS_PER_PRODUCER + i;

        assert(bq_push(arg->queue, &arg->records[index]) == 0);
        (void)atomic_fetch_add_explicit(arg->accepted, 1, memory_order_relaxed);
    }
    return NULL;
}

static void *mpmc_consumer(void *opaque)
{
    mpmc_consumer_arg_t *arg = opaque;
    record_t *record = NULL;
    int result;

    while ((result = bq_pop(arg->queue, &record)) == 1) {
        unsigned index;

        assert(record != NULL);
        assert(record->seq < TOTAL_ITEMS);
        index = (unsigned)record->seq;
        assert(atomic_fetch_add_explicit(&arg->seen[index], 1,
                                         memory_order_relaxed) == 0);
        (void)atomic_fetch_add_explicit(arg->observed, 1, memory_order_relaxed);
    }
    assert(result == 0);
    assert(record == NULL);
    return NULL;
}

static void *size_monitor(void *opaque)
{
    monitor_arg_t *arg = opaque;

    while (!atomic_load_explicit(&arg->stop, memory_order_acquire)) {
        if (bq_size(arg->queue) > MPMC_CAPACITY)
            atomic_store_explicit(&arg->exceeded_capacity, true,
                                  memory_order_relaxed);
        (void)atomic_fetch_add_explicit(&arg->samples, 1, memory_order_relaxed);
        (void)sched_yield();
    }
    return NULL;
}

static void test_many_producers_consumers_and_monitor(void)
{
    record_t records[TOTAL_ITEMS];
    atomic_uint seen[TOTAL_ITEMS];
    atomic_uint accepted;
    atomic_uint observed;
    mpmc_producer_arg_t producer_args[PRODUCER_COUNT];
    mpmc_consumer_arg_t consumer_args[CONSUMER_COUNT];
    pthread_t producers[PRODUCER_COUNT];
    pthread_t consumers[CONSUMER_COUNT];
    pthread_t monitor_thread;
    monitor_arg_t monitor;
    bq_t *queue = bq_create(MPMC_CAPACITY);

    assert(queue != NULL);
    atomic_init(&accepted, 0);
    atomic_init(&observed, 0);
    for (unsigned i = 0; i < TOTAL_ITEMS; i++) {
        records[i].seq = i;
        atomic_init(&seen[i], 0);
    }
    monitor.queue = queue;
    atomic_init(&monitor.stop, false);
    atomic_init(&monitor.exceeded_capacity, false);
    atomic_init(&monitor.samples, 0);

    for (unsigned i = 0; i < CONSUMER_COUNT; i++) {
        consumer_args[i].queue = queue;
        consumer_args[i].seen = seen;
        consumer_args[i].observed = &observed;
        assert(pthread_create(&consumers[i], NULL, mpmc_consumer,
                              &consumer_args[i]) == 0);
    }
    assert(pthread_create(&monitor_thread, NULL, size_monitor, &monitor) == 0);
    for (unsigned i = 0; i < PRODUCER_COUNT; i++) {
        producer_args[i].queue = queue;
        producer_args[i].records = records;
        producer_args[i].producer_id = i;
        producer_args[i].accepted = &accepted;
        assert(pthread_create(&producers[i], NULL, mpmc_producer,
                              &producer_args[i]) == 0);
    }

    for (unsigned i = 0; i < PRODUCER_COUNT; i++)
        join_with_timeout(producers[i], 10000);
    assert(atomic_load_explicit(&accepted, memory_order_relaxed) == TOTAL_ITEMS);
    bq_close(queue);
    for (unsigned i = 0; i < CONSUMER_COUNT; i++)
        join_with_timeout(consumers[i], 10000);
    atomic_store_explicit(&monitor.stop, true, memory_order_release);
    join_with_timeout(monitor_thread, 5000);

    assert(!atomic_load_explicit(&monitor.exceeded_capacity, memory_order_relaxed));
    assert(atomic_load_explicit(&monitor.samples, memory_order_relaxed) > 0);
    assert(atomic_load_explicit(&observed, memory_order_relaxed) == TOTAL_ITEMS);
    for (unsigned i = 0; i < TOTAL_ITEMS; i++)
        assert(atomic_load_explicit(&seen[i], memory_order_relaxed) == 1);
    assert(bq_size(queue) == 0);
    bq_destroy(queue);
}

static void test_fifo_close_and_errno(void)
{
    record_t records[3] = {
        { .seq = 10 },
        { .seq = 20 },
        { .seq = 30 }
    };
    record_t rejected = { .seq = 40 };
    record_t *out = NULL;

    errno = EDOM;
    bq_t *q = bq_create(3);
    assert(q != NULL);
    assert(errno == EDOM);

    errno = E2BIG;
    assert(bq_push(q, &records[0]) == 0);
    assert(errno == E2BIG);
    assert(bq_push(q, &records[1]) == 0);
    assert(bq_push(q, &records[2]) == 0);
    assert(errno == E2BIG);

    assert(bq_size(q) == 3);
    assert(errno == E2BIG);

    errno = ERANGE;
    bq_close(q);
    assert(errno == ERANGE);

    errno = ENOTTY;
    assert(bq_push(q, &rejected) == -1);
    assert(errno == ENOTTY);
    assert(rejected.seq == 40);

    errno = EDOM;
    assert(bq_pop(q, &out) == 1);
    assert(out == &records[0]);
    assert(errno == EDOM);
    assert(bq_pop(q, &out) == 1);
    assert(out == &records[1]);
    assert(bq_pop(q, &out) == 1);
    assert(out == &records[2]);
    assert(errno == EDOM);

    out = &records[0];
    assert(bq_pop(q, &out) == 0);
    assert(out == NULL);
    assert(errno == EDOM);
    assert(bq_size(q) == 0);

    bq_close(q);
    assert(errno == EDOM);
    bq_destroy(q);
    assert(errno == EDOM);
}

static void test_zero_capacity_rejected(void)
{
    errno = 0;
    assert(bq_create(0) == NULL);
    assert(errno == EINVAL);
}

static void test_destroy_does_not_free_queued_record(void)
{
    static record_t resident = { .seq = 99 };
    bq_t *q = bq_create(1);

    assert(q != NULL);
    assert(bq_push(q, &resident) == 0);
    bq_close(q);
    bq_destroy(q);
    assert(resident.seq == 99);
}

int main(void)
{
    test_fifo_close_and_errno();
    test_zero_capacity_rejected();
    test_destroy_does_not_free_queued_record();
    test_blocked_producers_resume_and_close();
    test_blocked_consumers_resume_and_close();
    test_many_producers_consumers_and_monitor();
    return 0;
}
