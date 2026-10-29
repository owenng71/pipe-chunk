#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "faults.h"
#include "io.h"

enum { DECISIONS = 2000 };

static config_t faults_config(double p_short, double p_eintr, double p_eagain, unsigned delay_us, unsigned long seed)
{
    return (config_t){ .fault_short = p_short, .fault_eintr = p_eintr, .fault_eagain = p_eagain,
                       .fault_delay_us = delay_us, .seed = seed };
}

/* One decision encoded as a number: -EINTR, -EAGAIN, or the allowed byte count. */
static long decide(fault_dir_t dir, size_t n)
{
    if (faults_before_io(dir, &n) < 0)
        return -(long)errno;
    return (long)n;
}

static void record(fault_dir_t dir, long *out, int count)
{
    for (int i = 0; i < count; i++)
        out[i] = decide(dir, 4096);
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void test_disabled_changes_nothing(void)
{
    config_t cfg = faults_config(0, 0, 0, 0, 7);

    faults_init(&cfg);
    for (size_t n = 0; n < 100; n++) {
        size_t m = n;

        errno = 12345;
        assert(faults_before_io(n % 2 ? FAULT_READ : FAULT_WRITE, &m) == 0);
        assert(m == n && errno == 12345);
    }
}

static void test_always_short(void)
{
    config_t cfg = faults_config(1, 0, 0, 0, 7);
    size_t n;

    faults_init(&cfg);
    for (size_t want = 2; want < 3000; want++) {
        n = want;
        assert(faults_before_io(FAULT_READ, &n) == 0);
        assert(n >= 1 && n < want);
    }
    n = 1;   // one byte cannot be shortened, zero bytes would look like EOF
    assert(faults_before_io(FAULT_WRITE, &n) == 0 && n == 1);
    n = 0;
    assert(faults_before_io(FAULT_WRITE, &n) == 0 && n == 0);
}

static void test_always_fail(void)
{
    config_t cfg = faults_config(0, 1, 0, 0, 7);
    size_t n = 100;

    faults_init(&cfg);
    assert(faults_before_io(FAULT_READ, &n) == -1 && errno == EINTR && n == 100);
    cfg = faults_config(0, 0, 1, 0, 7);
    faults_init(&cfg);
    assert(faults_before_io(FAULT_WRITE, &n) == -1 && errno == EAGAIN && n == 100);
}

static void test_probabilities(void)
{
    config_t cfg = faults_config(0.2, 0.3, 0.1, 0, 99);
    int eintr = 0, eagain = 0, shortened = 0, total = 20000;

    faults_init(&cfg);
    for (int i = 0; i < total; i++) {
        long d = decide(FAULT_READ, 4096);

        eintr += d == -EINTR;
        eagain += d == -EAGAIN;
        shortened += d >= 0 && d < 4096;
    }
    /* Checks run in order: EINTR, then EAGAIN on the rest, then short on what is left. */
    assert(eintr > total * 0.27 && eintr < total * 0.33);
    assert(eagain > total * 0.7 * 0.08 && eagain < total * 0.7 * 0.12);
    assert(shortened > total * 0.63 * 0.17 && shortened < total * 0.63 * 0.23);
}

static void test_reproducible(void)
{
    static long a[DECISIONS], b[DECISIONS], c[DECISIONS], reads[DECISIONS];
    config_t cfg = faults_config(0.3, 0.2, 0.2, 0, 42);

    faults_init(&cfg);
    record(FAULT_WRITE, a, DECISIONS);
    faults_init(&cfg);   // init restarts the streams
    record(FAULT_WRITE, b, DECISIONS);
    assert(memcmp(a, b, sizeof a) == 0);

    /* Reads in between do not shift the write stream. */
    faults_init(&cfg);
    for (int i = 0; i < DECISIONS; i++) {
        reads[i] = decide(FAULT_READ, 4096);
        c[i] = decide(FAULT_WRITE, 4096);
    }
    assert(memcmp(a, c, sizeof a) == 0);
    assert(memcmp(a, reads, sizeof a) != 0);   // the two directions are separate streams

    cfg.seed = 43;
    faults_init(&cfg);
    record(FAULT_WRITE, b, DECISIONS);
    assert(memcmp(a, b, sizeof a) != 0);
}

static void *record_writes(void *arg)
{
    record(FAULT_WRITE, arg, DECISIONS);
    return NULL;
}

/* A stream depends on the seed and direction, not on which thread asks. */
static void test_same_stream_in_another_thread(void)
{
    static long mine[DECISIONS], theirs[DECISIONS];
    config_t cfg = faults_config(0.3, 0.2, 0.2, 0, 5);
    pthread_t t;

    faults_init(&cfg);
    record(FAULT_WRITE, mine, DECISIONS);
    assert(pthread_create(&t, NULL, record_writes, theirs) == 0);
    assert(pthread_join(t, NULL) == 0);
    assert(memcmp(mine, theirs, sizeof mine) == 0);
}

static void test_delay(void)
{
    config_t cfg = faults_config(0, 0, 0, 2000, 3);
    uint64_t t0, elapsed;
    size_t n = 10;

    faults_init(&cfg);
    t0 = now_ns();
    for (int i = 0; i < 50; i++)
        assert(faults_before_io(FAULT_READ, &n) == 0 && n == 10);
    elapsed = now_ns() - t0;
    /* Uniform 0..2 ms per call: about 50 ms in total, at most 100 ms plus scheduling slack. */
    assert(elapsed > 15 * 1000000u && elapsed < 600 * 1000000u);
}

/* With every fault on, read_full and pwrite_full still move every byte to the right place. */
static void test_io_survives_faults(void)
{
    char path[] = "/tmp/test_faults_XXXXXX";
    int fd = mkstemp(path);
    size_t size = 300000;
    unsigned char *data = malloc(size), *back = calloc(1, size);
    config_t cfg = faults_config(0.5, 0.3, 0.3, 20, 11);

    assert(fd >= 0 && data != NULL && back != NULL);
    for (size_t i = 0; i < size; i++)
        data[i] = (unsigned char)(i * 7u + (i >> 8));
    faults_init(&cfg);
    for (size_t off = 0; off < size; off += 30000)   // out of order, like the pipeline writer
        assert(pwrite_full(fd, data + (size - 30000 - off), 30000, (off_t)(size - 30000 - off)) == 0);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    assert(read_full(fd, back, size) == (ssize_t)size);
    assert(read_full(fd, back, 10) == 0);   // EOF still reported as 0
    assert(memcmp(data, back, size) == 0);
    close(fd);
    unlink(path);
    free(data);
    free(back);
}

int main(void)
{
    test_disabled_changes_nothing();
    test_always_short();
    test_always_fail();
    test_probabilities();
    test_reproducible();
    test_same_stream_in_another_thread();
    test_delay();
    test_io_survives_faults();

    config_t off = faults_config(0, 0, 0, 0, 1);
    faults_init(&off);
    return 0;
}
