#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "pool.h"

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) != 0)
        assert(errno == EINTR);
}

static void test_create_rejects_bad_sizes(void)
{
    errno = 0;
    assert(pool_create(4, 0) == NULL);
    assert(errno == EINVAL);
    errno = 0;
    assert(pool_create(SIZE_MAX / 2, 4) == NULL);
    assert(errno == EOVERFLOW);
}

static void test_trays_are_distinct_clean_and_sized(void)
{
    enum { N = 8, SIZE = 1000 };
    record_t *r[N];
    pool_t *p;

    errno = ENOTTY;
    p = pool_create(N, SIZE);
    assert(p != NULL);
    assert(errno == ENOTTY);

    for (int i = 0; i < N; i++) {
        r[i] = pool_get(p);
        assert(r[i] != NULL);
        assert(r[i]->cap == SIZE && r[i]->len == 0 && r[i]->seq == 0 && r[i]->attempts == 0);
        memset(r[i]->buf, i, SIZE);   // whole buffer is writable (ASan checks the bounds)
    }
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++)
            assert(i == j || (r[i] != r[j] && r[i]->buf != r[j]->buf));
        for (int k = 0; k < SIZE; k++)
            assert(r[i]->buf[k] == (unsigned char)i);   // no buffer overlaps another
    }
    for (int i = 0; i < N; i++)
        pool_put(p, r[i]);
    pool_destroy(p);
}

static void test_get_resets_metadata_and_restores_buf(void)
{
    static unsigned char other[16];
    pool_t *p = pool_create(1, 64);
    record_t *r;
    unsigned char *own;

    assert(p != NULL);
    r = pool_get(p);
    own = r->buf;
    r->seq = 7;
    r->offset = 4096;
    r->len = 10;
    r->sha[0] = 0xab;
    r->t_read_ns = 99;
    r->attempts = 2;
    r->buf = other;   // e.g. pointed into an mmap'ed input
    r->cap = sizeof other;
    pool_put(p, r);

    r = pool_get(p);
    assert(r->seq == 0 && r->offset == 0 && r->len == 0 && r->sha[0] == 0);
    assert(r->t_read_ns == 0 && r->attempts == 0);
    assert(r->buf == own && r->cap == 64);
    pool_put(p, r);
    pool_destroy(p);
}

typedef struct {
    pool_t *pool;
    record_t *got;
    atomic_bool done;
} getter_t;

static void *getter(void *arg)
{
    getter_t *g = arg;

    g->got = pool_get(g->pool);
    atomic_store(&g->done, true);
    return NULL;
}

static void test_get_blocks_until_put(void)
{
    pool_t *p = pool_create(2, 32);
    record_t *a, *b;
    getter_t g = { .pool = p };
    pthread_t t;

    assert(p != NULL);
    a = pool_get(p);
    b = pool_get(p);
    assert(pthread_create(&t, NULL, getter, &g) == 0);
    sleep_ms(50);
    assert(!atomic_load(&g.done));   // every tray is out: the getter must wait

    pool_put(p, b);
    assert(pthread_join(t, NULL) == 0);
    assert(g.got == b);
    pool_put(p, a);
    pool_put(p, g.got);
    pool_destroy(p);
}

static void test_close_wakes_getters_and_put_still_works(void)
{
    enum { T = 4 };
    pool_t *p = pool_create(1, 32);
    getter_t g[T];
    pthread_t t[T];
    record_t *held;

    assert(p != NULL);
    held = pool_get(p);
    for (int i = 0; i < T; i++) {
        g[i] = (getter_t){ .pool = p };
        assert(pthread_create(&t[i], NULL, getter, &g[i]) == 0);
    }
    sleep_ms(50);
    for (int i = 0; i < T; i++)
        assert(!atomic_load(&g[i].done));

    errno = ENOTTY;
    pool_close(p);
    assert(errno == ENOTTY);
    for (int i = 0; i < T; i++) {
        assert(pthread_join(t[i], NULL) == 0);
        assert(g[i].got == NULL);
    }

    /* After close the writer still returns trays, but nobody gets one. */
    errno = ENOTTY;
    pool_put(p, held);
    assert(errno == ENOTTY);
    assert(pool_get(p) == NULL);
    pool_close(p);   // idempotent
    pool_destroy(p);
}

static void test_put_rejects_foreign_and_double_put(void)
{
    enum { N = 3 };
    pool_t *p = pool_create(N, 16);
    pool_t *q = pool_create(1, 16);
    record_t local = { 0 };
    record_t *r[N], *foreign;

    assert(p != NULL && q != NULL);
    for (int i = 0; i < N; i++)
        r[i] = pool_get(p);
    foreign = pool_get(q);

    pool_put(p, r[0]);
    errno = 0;
    pool_put(p, r[0]);   // double put
    assert(errno == EINVAL);
    errno = 0;
    pool_put(p, &local);
    assert(errno == EINVAL);
    errno = 0;
    pool_put(p, foreign);
    assert(errno == EINVAL);
    errno = 0;
    pool_put(p, NULL);
    assert(errno == EINVAL);

    /* Only one free tray: if the double put had been accepted, a second getter would get r[0] too. */
    r[0] = pool_get(p);
    {
        getter_t g = { .pool = p };
        pthread_t t;

        assert(pthread_create(&t, NULL, getter, &g) == 0);
        sleep_ms(50);
        assert(!atomic_load(&g.done));
        pool_close(p);
        assert(pthread_join(t, NULL) == 0);
        assert(g.got == NULL);
    }
    for (int i = 0; i < N; i++)
        pool_put(p, r[i]);
    pool_put(q, foreign);
    pool_destroy(p);
    pool_destroy(q);
}

static void test_unbounded_never_blocks(void)
{
    enum { N = 1000, SIZE = 100 };
    static record_t *r[N];
    record_t local = { 0 };
    pool_t *p = pool_create(0, SIZE);

    assert(p != NULL);
    for (int i = 0; i < N; i++) {   // far past the first chunk: the pool grows instead of waiting
        r[i] = pool_get(p);
        assert(r[i] != NULL && r[i]->cap == SIZE);
        memset(r[i]->buf, i & 0xff, SIZE);
    }
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < SIZE; k++)
            assert(r[i]->buf[k] == (unsigned char)(i & 0xff));   // buffers never overlap
    }

    pool_put(p, r[N - 1]);
    errno = 0;
    pool_put(p, r[N - 1]);   // double put
    assert(errno == EINVAL);
    errno = 0;
    pool_put(p, &local);
    assert(errno == EINVAL);
    assert(pool_get(p) == r[N - 1]);   // a returned tray is reused before growing again

    for (int i = 0; i < N / 2; i++)
        pool_put(p, r[i]);
    pool_close(p);
    assert(pool_get(p) == NULL);
    pool_destroy(p);   // half the trays still checked out: ASan's leak check covers all chunks
}

static void test_destroy_with_trays_checked_out(void)
{
    pool_t *p = pool_create(4, 128);

    assert(p != NULL);
    assert(pool_get(p) != NULL);
    assert(pool_get(p) != NULL);
    pool_destroy(p);   // ASan's leak check proves every tray and buffer is freed
}

enum { STRESS_THREADS = 8, STRESS_TRAYS = 3, STRESS_ROUNDS = 20000, STRESS_SIZE = 64 };

typedef struct {
    pool_t *pool;
    unsigned char id;
} stresser_t;

static atomic_int in_use;
static atomic_int max_in_use;

static void *stresser(void *arg)
{
    stresser_t *s = arg;

    for (int round = 0; round < STRESS_ROUNDS; round++) {
        record_t *r = pool_get(s->pool);
        int now = atomic_fetch_add(&in_use, 1) + 1;
        int seen = atomic_load(&max_in_use);

        assert(r != NULL);
        while (now > seen && !atomic_compare_exchange_weak(&max_in_use, &seen, now))
            ;
        memset(r->buf, s->id, STRESS_SIZE);
        r->seq = s->id;
        if (round % 64 == 0)
            sched_yield();
        for (int k = 0; k < STRESS_SIZE; k++)
            assert(r->buf[k] == s->id);   // nobody else owns this tray
        assert(r->seq == s->id);
        atomic_fetch_sub(&in_use, 1);
        pool_put(s->pool, r);
    }
    return NULL;
}

static void test_stress_one_owner_per_tray(void)
{
    pool_t *p = pool_create(STRESS_TRAYS, STRESS_SIZE);
    stresser_t s[STRESS_THREADS];
    pthread_t t[STRESS_THREADS];

    assert(p != NULL);
    for (int i = 0; i < STRESS_THREADS; i++) {
        s[i] = (stresser_t){ .pool = p, .id = (unsigned char)(i + 1) };
        assert(pthread_create(&t[i], NULL, stresser, &s[i]) == 0);
    }
    for (int i = 0; i < STRESS_THREADS; i++)
        assert(pthread_join(t[i], NULL) == 0);
    assert(atomic_load(&max_in_use) <= STRESS_TRAYS);   // memory stays bounded
    pool_destroy(p);
}

int main(void)
{
    test_create_rejects_bad_sizes();
    test_trays_are_distinct_clean_and_sized();
    test_get_resets_metadata_and_restores_buf();
    test_get_blocks_until_put();
    test_close_wakes_getters_and_put_still_works();
    test_put_rejects_foreign_and_double_put();
    test_unbounded_never_blocks();
    test_destroy_with_trays_checked_out();
    test_stress_one_owner_per_tray();
    return 0;
}
