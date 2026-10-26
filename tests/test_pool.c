#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pool.h"

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#include <malloc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

enum { JOIN_TIMEOUT_MS = 10000, BLOCK_CHECK_MS = 150 };

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned ready;
    unsigned done;
} cohort_t;

static void cohort_init(cohort_t *cohort)
{
    assert(pthread_mutex_init(&cohort->mutex, NULL) == 0);
    assert(pthread_cond_init(&cohort->cond, NULL) == 0);
    cohort->ready = 0;
    cohort->done = 0;
}

static void cohort_destroy(cohort_t *cohort)
{
    assert(pthread_cond_destroy(&cohort->cond) == 0);
    assert(pthread_mutex_destroy(&cohort->mutex) == 0);
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

static void cohort_mark_ready(cohort_t *cohort)
{
    assert(pthread_mutex_lock(&cohort->mutex) == 0);
    cohort->ready++;
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

static void cohort_wait_ready(cohort_t *cohort, unsigned expected)
{
    struct timespec deadline = deadline_after_ms(JOIN_TIMEOUT_MS);
    int rc = 0;

    assert(pthread_mutex_lock(&cohort->mutex) == 0);
    while (cohort->ready < expected && rc == 0)
        rc = pthread_cond_timedwait(&cohort->cond, &cohort->mutex, &deadline);
    assert(cohort->ready >= expected);
    assert(pthread_mutex_unlock(&cohort->mutex) == 0);
}

static void cohort_assert_blocked(cohort_t *cohort, long milliseconds)
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

static void cohort_wait_done(cohort_t *cohort, unsigned expected)
{
    struct timespec deadline = deadline_after_ms(JOIN_TIMEOUT_MS);
    int rc = 0;

    assert(pthread_mutex_lock(&cohort->mutex) == 0);
    while (cohort->done < expected && rc == 0)
        rc = pthread_cond_timedwait(&cohort->cond, &cohort->mutex, &deadline);
    assert(cohort->done >= expected);
    assert(pthread_mutex_unlock(&cohort->mutex) == 0);
}

static void join_with_timeout(pthread_t thread)
{
    struct timespec deadline = deadline_after_ms(JOIN_TIMEOUT_MS);

    assert(pthread_timedjoin_np(thread, NULL, &deadline) == 0);
}

typedef struct {
    pool_t *pool;
    cohort_t *cohort;
    record_t *result;
} getter_arg_t;

static void *pool_getter(void *opaque)
{
    getter_arg_t *arg = opaque;

    cohort_mark_ready(arg->cohort);
    arg->result = pool_get(arg->pool);
    cohort_mark_done(arg->cohort);
    return NULL;
}

static void test_create_distinct_trays_and_buffers(void)
{
    enum { NBUFS = 4, BUFSIZE = 128 };
    record_t *trays[NBUFS];
    pool_t *pool;

    errno = EDOM;
    pool = pool_create(NBUFS, BUFSIZE);
    assert(pool != NULL);
    assert(errno == EDOM);

    for (size_t i = 0; i < NBUFS; i++) {
        trays[i] = pool_get(pool);
        assert(trays[i] != NULL);
        assert(trays[i]->buf != NULL);
        assert(trays[i]->cap == BUFSIZE);
        for (size_t j = 0; j < i; j++) {
            assert(trays[i] != trays[j]);
            assert(trays[i]->buf != trays[j]->buf);
        }
    }
    for (size_t i = 0; i < NBUFS; i++)
        pool_put(pool, trays[i]);
    pool_destroy(pool);
}

static void test_exhausted_getter_reuses_returned_tray(void)
{
    record_t *first;
    record_t *second;
    record_t *returned;
    getter_arg_t arg;
    cohort_t cohort;
    pthread_t thread;
    pool_t *pool = pool_create(2, 64);

    assert(pool != NULL);
    first = pool_get(pool);
    second = pool_get(pool);
    assert(first != NULL && second != NULL && first != second);
    cohort_init(&cohort);
    arg.pool = pool;
    arg.cohort = &cohort;
    arg.result = NULL;
    assert(pthread_create(&thread, NULL, pool_getter, &arg) == 0);
    cohort_wait_ready(&cohort, 1);
    cohort_assert_blocked(&cohort, BLOCK_CHECK_MS);

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    struct mallinfo2 before = mallinfo2();
    size_t before_in_use = before.uordblks + before.hblkhd;
#endif
    pool_put(pool, second);
    cohort_wait_done(&cohort, 1);
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    struct mallinfo2 after = mallinfo2();
    assert(after.uordblks + after.hblkhd == before_in_use);
#endif
    join_with_timeout(thread);
    returned = arg.result;
    assert(returned == second);
    assert(returned != first);
    pool_put(pool, returned);
    pool_put(pool, first);
    cohort_destroy(&cohort);
    pool_destroy(pool);
}

static void test_waiting_getter_takes_returned_tray(void)
{
    record_t *tray;
    getter_arg_t arg;
    cohort_t cohort;
    pthread_t thread;
    pool_t *pool = pool_create(1, 32);

    assert(pool != NULL);
    tray = pool_get(pool);
    assert(tray != NULL);
    cohort_init(&cohort);
    arg.pool = pool;
    arg.cohort = &cohort;
    arg.result = NULL;
    assert(pthread_create(&thread, NULL, pool_getter, &arg) == 0);
    cohort_wait_ready(&cohort, 1);
    cohort_assert_blocked(&cohort, BLOCK_CHECK_MS);

    pool_put(pool, tray);
    cohort_wait_done(&cohort, 1);
    join_with_timeout(thread);
    assert(arg.result == tray);
    pool_put(pool, arg.result);
    cohort_destroy(&cohort);
    pool_destroy(pool);
}

static void test_checkout_resets_metadata_and_restores_buf(void)
{
    unsigned char external[32] = { 0 };
    record_t *tray;
    unsigned char *original;
    pool_t *pool = pool_create(1, sizeof external);

    assert(pool != NULL);
    tray = pool_get(pool);
    assert(tray != NULL);
    original = tray->buf;
    tray->seq = 91;
    tray->offset = 123;
    tray->len = 17;
    memset(tray->sha, 0xa5, sizeof tray->sha);
    tray->t_read_ns = 456;
    tray->attempts = 7;
    tray->buf = external;
    tray->cap = 9;
    pool_put(pool, tray);

    tray = pool_get(pool);
    assert(tray != NULL);
    assert(tray->seq == 0);
    assert(tray->offset == 0);
    assert(tray->len == 0);
    for (size_t i = 0; i < sizeof tray->sha; i++)
        assert(tray->sha[i] == 0);
    assert(tray->t_read_ns == 0);
    assert(tray->attempts == 0);
    assert(tray->buf == original);
    assert(tray->cap == sizeof external);
    pool_put(pool, tray);
    pool_destroy(pool);
}

static void test_close_wakes_all_blocked_getters(void)
{
    enum { WAITER_COUNT = 5 };
    getter_arg_t args[WAITER_COUNT];
    pthread_t threads[WAITER_COUNT];
    cohort_t cohort;
    record_t *held;
    pool_t *pool = pool_create(1, 32);

    assert(pool != NULL);
    held = pool_get(pool);
    assert(held != NULL);
    cohort_init(&cohort);
    for (unsigned i = 0; i < WAITER_COUNT; i++) {
        args[i].pool = pool;
        args[i].cohort = &cohort;
        args[i].result = (record_t *)(uintptr_t)1;
        assert(pthread_create(&threads[i], NULL, pool_getter, &args[i]) == 0);
    }
    cohort_wait_ready(&cohort, WAITER_COUNT);
    cohort_assert_blocked(&cohort, BLOCK_CHECK_MS);

    pool_close(pool);
    cohort_wait_done(&cohort, WAITER_COUNT);
    for (unsigned i = 0; i < WAITER_COUNT; i++) {
        join_with_timeout(threads[i]);
        assert(args[i].result == NULL);
    }
    pool_put(pool, held);
    cohort_destroy(&cohort);
    pool_destroy(pool);
}

typedef struct {
    pool_t *pool;
    cohort_t *cohort;
    int errno_after;
} closer_arg_t;

static void *pool_closer(void *opaque)
{
    closer_arg_t *arg = opaque;

    cohort_mark_ready(arg->cohort);
    errno = EACCES;
    pool_close(arg->pool);
    arg->errno_after = errno;
    cohort_mark_done(arg->cohort);
    return NULL;
}

static void test_close_precedes_free_trays_and_is_concurrent(void)
{
    enum { CLOSER_COUNT = 3 };
    closer_arg_t args[CLOSER_COUNT];
    pthread_t threads[CLOSER_COUNT];
    cohort_t cohort;
    pool_t *pool = pool_create(2, 32);

    assert(pool != NULL);
    cohort_init(&cohort);
    for (unsigned i = 0; i < CLOSER_COUNT; i++) {
        args[i].pool = pool;
        args[i].cohort = &cohort;
        args[i].errno_after = 0;
        assert(pthread_create(&threads[i], NULL, pool_closer, &args[i]) == 0);
    }
    cohort_wait_ready(&cohort, CLOSER_COUNT);
    cohort_wait_done(&cohort, CLOSER_COUNT);
    for (unsigned i = 0; i < CLOSER_COUNT; i++) {
        join_with_timeout(threads[i]);
        assert(args[i].errno_after == EACCES);
    }
    errno = ERANGE;
    assert(pool_get(pool) == NULL);
    assert(errno == ERANGE);
    pool_close(pool);
    assert(errno == ERANGE);
    cohort_destroy(&cohort);
    pool_destroy(pool);
}

static void test_return_after_close_is_retained(void)
{
    record_t *tray;
    pool_t *pool = pool_create(1, 32);

    assert(pool != NULL);
    tray = pool_get(pool);
    assert(tray != NULL);
    pool_close(pool);
    errno = EDOM;
    pool_put(pool, tray);
    assert(errno == EDOM);
    assert(pool_get(pool) == NULL);
    pool_destroy(pool);
}

enum { STRESS_TRAYS = 8, STRESS_THREADS = 6, STRESS_ROUNDS = 1500,
       SINGLE_TRAY_THREADS = 8 };

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
enum { SINGLE_TRAY_ROUNDS = 2000 };
#else
enum { SINGLE_TRAY_ROUNDS = 20000 };
#endif

typedef struct {
    pool_t *pool;
    record_t **known_trays;
    size_t tray_count;
    atomic_bool *active;
    atomic_uint *operations;
    unsigned rounds;
    unsigned thread_count;
    bool yield_while_held;
} stress_arg_t;

static size_t tray_index(record_t **trays, size_t tray_count, record_t *tray)
{
    for (size_t i = 0; i < tray_count; i++) {
        if (trays[i] == tray)
            return i;
    }
    assert(0 && "pool returned a foreign tray");
    return 0;
}

static void *pool_stress_worker(void *opaque)
{
    stress_arg_t *arg = opaque;

    for (unsigned iteration = 0; iteration < arg->rounds; iteration++) {
        record_t *tray = pool_get(arg->pool);
        size_t index;

        assert(tray != NULL);
        index = tray_index(arg->known_trays, arg->tray_count, tray);
        assert(!atomic_exchange_explicit(&arg->active[index], true,
                                        memory_order_acq_rel));
        tray->seq = iteration;
        assert(atomic_fetch_add_explicit(arg->operations, 1,
                                         memory_order_relaxed) <
               arg->thread_count * arg->rounds);
        if (arg->yield_while_held)
            (void)sched_yield();
        atomic_store_explicit(&arg->active[index], false, memory_order_release);
        pool_put(arg->pool, tray);
    }
    return NULL;
}

static void test_concurrent_get_put_ownership(void)
{
    record_t *known[STRESS_TRAYS];
    atomic_bool active[STRESS_TRAYS];
    atomic_uint operations;
    stress_arg_t args[STRESS_THREADS];
    pthread_t threads[STRESS_THREADS];
    pool_t *pool = pool_create(STRESS_TRAYS, 64);

    assert(pool != NULL);
    atomic_init(&operations, 0);
    for (size_t i = 0; i < STRESS_TRAYS; i++) {
        known[i] = pool_get(pool);
        assert(known[i] != NULL);
        atomic_init(&active[i], false);
    }
    for (size_t i = 0; i < STRESS_TRAYS; i++)
        pool_put(pool, known[i]);
    for (unsigned i = 0; i < STRESS_THREADS; i++) {
        args[i].pool = pool;
        args[i].known_trays = known;
        args[i].tray_count = STRESS_TRAYS;
        args[i].active = active;
        args[i].operations = &operations;
        args[i].rounds = STRESS_ROUNDS;
        args[i].thread_count = STRESS_THREADS;
        args[i].yield_while_held = false;
        assert(pthread_create(&threads[i], NULL, pool_stress_worker,
                              &args[i]) == 0);
    }
    for (unsigned i = 0; i < STRESS_THREADS; i++)
        join_with_timeout(threads[i]);
    assert(atomic_load_explicit(&operations, memory_order_relaxed) ==
           STRESS_THREADS * STRESS_ROUNDS);
    for (size_t i = 0; i < STRESS_TRAYS; i++) {
        assert(!atomic_load_explicit(&active[i], memory_order_relaxed));
    }
    pool_destroy(pool);
}

static void test_single_tray_wait_uses_predicate_loop(void)
{
    record_t *known[1];
    atomic_bool active[1];
    atomic_uint operations;
    stress_arg_t args[SINGLE_TRAY_THREADS];
    pthread_t threads[SINGLE_TRAY_THREADS];
    pool_t *pool = pool_create(1, 64);

    assert(pool != NULL);
    known[0] = pool_get(pool);
    assert(known[0] != NULL);
    atomic_init(&active[0], false);
    atomic_init(&operations, 0);
    pool_put(pool, known[0]);

    for (unsigned i = 0; i < SINGLE_TRAY_THREADS; i++) {
        args[i].pool = pool;
        args[i].known_trays = known;
        args[i].tray_count = 1;
        args[i].active = active;
        args[i].operations = &operations;
        args[i].rounds = SINGLE_TRAY_ROUNDS;
        args[i].thread_count = SINGLE_TRAY_THREADS;
        args[i].yield_while_held = true;
        assert(pthread_create(&threads[i], NULL, pool_stress_worker,
                              &args[i]) == 0);
    }
    for (unsigned i = 0; i < SINGLE_TRAY_THREADS; i++)
        join_with_timeout(threads[i]);

    assert(atomic_load_explicit(&operations, memory_order_relaxed) ==
           SINGLE_TRAY_THREADS * SINGLE_TRAY_ROUNDS);
    assert(!atomic_load_explicit(&active[0], memory_order_relaxed));
    pool_destroy(pool);
}

static void test_zero_sizes_are_rejected(void)
{
    errno = 0;
    assert(pool_create(0, 64) == NULL);
    assert(errno == EINVAL);
    errno = 0;
    assert(pool_create(2, 0) == NULL);
    assert(errno == EINVAL);
}

static void test_destroy_frees_original_for_checked_out_tray(void)
{
    unsigned char external[64] = { 0 };
    record_t *tray;
    pool_t *pool = pool_create(1, sizeof external);

    assert(pool != NULL);
    tray = pool_get(pool);
    assert(tray != NULL);
    tray->buf = external;
    pool_destroy(pool);
    external[0] = 0x5a;
    assert(external[0] == 0x5a);
}

static void test_foreign_and_duplicate_put_are_rejected(void)
{
    record_t foreign = { 0 };
    getter_arg_t arg;
    cohort_t cohort;
    pthread_t thread;
    record_t *tray;
    pool_t *pool = pool_create(1, 32);

    assert(pool != NULL);
    tray = pool_get(pool);
    assert(tray != NULL);

    errno = 0;
    pool_put(pool, &foreign);
    assert(errno == EINVAL);
    errno = 0;
    pool_put(pool, (record_t *)((uintptr_t)tray + 1));
    assert(errno == EINVAL);
    errno = 0;
    pool_put(pool, (record_t *)((uintptr_t)tray + sizeof *tray));
    assert(errno == EINVAL);

    pool_put(pool, tray);
    errno = 0;
    pool_put(pool, tray);
    assert(errno == EINVAL);
    assert(pool_get(pool) == tray);

    cohort_init(&cohort);
    arg.pool = pool;
    arg.cohort = &cohort;
    arg.result = NULL;
    assert(pthread_create(&thread, NULL, pool_getter, &arg) == 0);
    cohort_wait_ready(&cohort, 1);
    cohort_assert_blocked(&cohort, BLOCK_CHECK_MS);
    pool_close(pool);
    cohort_wait_done(&cohort, 1);
    join_with_timeout(thread);
    assert(arg.result == NULL);
    cohort_destroy(&cohort);
    pool_destroy(pool);
}

static void test_destroy_does_not_free_repointed_buf(void)
{
    unsigned char external[16] = { 0 };
    record_t *tray;
    pool_t *pool = pool_create(1, 16);

    assert(pool != NULL);
    tray = pool_get(pool);
    assert(tray != NULL);
    tray->buf = external;
    pool_destroy(pool);
    external[0] = 0xa5;
    assert(external[0] == 0xa5);
}

static void test_size_overflow_and_partial_enomem_cleanup(void)
{
    errno = 0;
    assert(pool_create(SIZE_MAX / sizeof(record_t) + 1, 1) == NULL);
    assert(errno == EOVERFLOW);
    errno = 0;
    assert(pool_create(2, SIZE_MAX / 2 + 1) == NULL);
    assert(errno == EOVERFLOW);

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    /* Sanitizer address-space reservations make RLIMIT_AS non-deterministic. */
#else
    struct rlimit old_limit;
    struct rlimit test_limit;
    struct rlimit restored_limit;
    struct mallinfo2 before;
    struct mallinfo2 after;
    long page_size = sysconf(_SC_PAGESIZE);
    unsigned long pages = 0;
    unsigned long long virtual_bytes;
    unsigned long long allowance = 8ULL * 1024ULL * 1024ULL;
    unsigned long long target;
    FILE *statm;
    pool_t *warmup_pool;
    pool_t *failed_pool;
    int failure_errno;

    assert(page_size > 0);
    statm = fopen("/proc/self/statm", "r");
    assert(statm != NULL);
    assert(fscanf(statm, "%lu", &pages) == 1);
    assert(fclose(statm) == 0);
    virtual_bytes = (unsigned long long)pages * (unsigned long long)page_size;
    assert(getrlimit(RLIMIT_AS, &old_limit) == 0);
    target = virtual_bytes + allowance;
    if (old_limit.rlim_max != RLIM_INFINITY &&
        target > (unsigned long long)old_limit.rlim_max)
        target = (unsigned long long)old_limit.rlim_max;
    assert(target > virtual_bytes + 2ULL * 1024ULL * 1024ULL);
    test_limit = old_limit;
    test_limit.rlim_cur = (rlim_t)target;
    assert(setrlimit(RLIMIT_AS, &test_limit) == 0);

    /* Prime the libc allocator's first-failure bookkeeping before the measured snapshot. */
    errno = 0;
    warmup_pool = pool_create(64, 1024U * 1024U);
    assert(warmup_pool == NULL);
    assert(errno == ENOMEM);
    before = mallinfo2();
    errno = 0;
    failed_pool = pool_create(64, 1024U * 1024U);
    failure_errno = errno;
    if (failed_pool != NULL)
        pool_destroy(failed_pool);

    restored_limit = old_limit;
    assert(setrlimit(RLIMIT_AS, &restored_limit) == 0);
    after = mallinfo2();
    assert(failed_pool == NULL);
    assert(failure_errno == ENOMEM);
    assert(after.uordblks + after.hblkhd == before.uordblks + before.hblkhd);

    pool_t *small = pool_create(2, 64);
    assert(small != NULL);
    pool_destroy(small);
#endif
}

int main(void)
{
    test_create_distinct_trays_and_buffers();
    test_exhausted_getter_reuses_returned_tray();
    test_waiting_getter_takes_returned_tray();
    test_checkout_resets_metadata_and_restores_buf();
    test_close_wakes_all_blocked_getters();
    test_close_precedes_free_trays_and_is_concurrent();
    test_return_after_close_is_retained();
    test_concurrent_get_put_ownership();
    test_single_tray_wait_uses_predicate_loop();
    test_zero_sizes_are_rejected();
    test_destroy_frees_original_for_checked_out_tray();
    test_foreign_and_duplicate_put_are_rejected();
    test_destroy_does_not_free_repointed_buf();
    test_size_overflow_and_partial_enomem_cleanup();
    return 0;
}
