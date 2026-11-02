#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "metrics.h"

static char dir[] = "/tmp/test_metrics_XXXXXX";

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) != 0)
        assert(errno == EINTR);
}

/* Reads line `want` (0-based) of PREFIX SUFFIX into buf; returns the file's line count. */
static int read_line(const char *prefix, const char *suffix, int want, char *buf, size_t size)
{
    char path[256], line[512];
    FILE *f;
    int n = 0;

    snprintf(path, sizeof path, "%s%s", prefix, suffix);
    f = fopen(path, "r");
    assert(f != NULL);
    while (fgets(line, sizeof line, f) != NULL) {
        if (n == want || want < 0)
            snprintf(buf, size, "%s", line);   // want < 0: keep the last line
        n++;
    }
    assert(fclose(f) == 0);
    return n;
}

static void test_now_ns(void)
{
    uint64_t a, b;

    assert(metrics_now_ns(&a) == 0);
    sleep_ms(2);
    assert(metrics_now_ns(&b) == 0);
    assert(b - a >= 2000000u);
    errno = 0;
    assert(metrics_now_ns(NULL) == -1);
    assert(errno == EINVAL);
}

static void test_disabled_collects_nothing(void)
{
    metrics_t *m = metrics_create(false, NULL);
    metrics_summary_t s = { .bytes = 10, .blocks = 1, .wall_ns = 1000 };

    assert(m != NULL);
    assert(metrics_thread_register(m) == 0);
    metrics_io_add(12345);
    assert(metrics_record_block(m, 1, 2) == 0);
    assert(metrics_monitor_start(m, NULL, NULL, 0) == 0);
    assert(metrics_monitor_stop(m) == 0);
    assert(metrics_merge(m, &s) == 0);
    assert(!s.have_latency && !s.have_io_call && s.have_resources);
    assert(s.bytes == 10 && s.blocks == 1 && s.wall_ns == 1000);   // caller totals are untouched
    assert(metrics_write_csv(m, &s) == 0);
    metrics_thread_unregister();
    metrics_destroy(m);
}

typedef struct {
    metrics_t *m;
    uint64_t ns;
} io_adder_t;

static void *io_adder(void *arg)
{
    io_adder_t *a = arg;

    assert(metrics_thread_register(a->m) == 0);
    metrics_io_add(a->ns);
    metrics_thread_unregister();
    metrics_io_add(1000000);   // after unregister: ignored
    return NULL;
}

static void test_enabled_full_run(void)
{
    char prefix[64], line[512];
    metrics_t *m;
    metrics_summary_t s = { .bytes = 2000000, .blocks = 100, .wall_ns = 1000000000u };
    io_adder_t adder;
    pthread_t t;
    bq_t *q1 = bq_create(8), *q2 = bq_create(8);
    record_t r[3];
    uint64_t origin;
    int rows;

    snprintf(prefix, sizeof prefix, "%s/run", dir);
    m = metrics_create(true, prefix);
    assert(m != NULL && q1 != NULL && q2 != NULL);

    assert(metrics_thread_register(m) == 0);
    errno = 0;
    assert(metrics_thread_register(m) == -1);
    assert(errno == EBUSY);
    metrics_io_add(500);
    adder = (io_adder_t){ m, 700 };
    assert(pthread_create(&t, NULL, io_adder, &adder) == 0);
    assert(pthread_join(t, NULL) == 0);

    for (uint64_t i = 1; i <= 100; i++)   // latencies 1..100 us
        assert(metrics_record_block(m, 5000, 5000 + i * 1000) == 0);
    errno = 0;
    assert(metrics_record_block(m, 10, 9) == -1);
    assert(errno == EINVAL);

    assert(metrics_now_ns(&origin) == 0);
    assert(metrics_monitor_start(m, q1, q2, origin) == 0);
    for (int i = 0; i < 3; i++)
        assert(bq_push(q1, &r[i]) == 0);
    sleep_ms(60);
    assert(metrics_monitor_stop(m) == 0);

    assert(metrics_merge(m, &s) == 0);
    assert(s.have_io_call && s.io_call_ns == 1200);
    assert(s.have_latency && s.lat_p50_ns == 50000 && s.lat_p99_ns == 99000);
    assert(s.have_resources && s.peak_rss_kb > 0);
    assert(metrics_write_csv(m, &s) == 0);
    metrics_print_summary(&s);

    assert(read_line(prefix, ".summary.csv", 0, line, sizeof line) == 2);
    assert(strcmp(line, "bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,"
                        "cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s\n") == 0);
    read_line(prefix, ".summary.csv", 1, line, sizeof line);
    assert(starts_with(line, "2000000,100,1.000000,2.000,50.0,99.0,"));
    assert(strstr(line, ",0.000001\n") != NULL);   // io_call_s = 1200 ns

    rows = read_line(prefix, ".occupancy.csv", 0, line, sizeof line);
    assert(strcmp(line, "t_ms,q1_size,q2_size\n") == 0);
    assert(rows >= 1 + 4);   // ~60 ms at 10 ms per sample, plus the final one
    read_line(prefix, ".occupancy.csv", -1, line, sizeof line);
    assert(strstr(line, ",3,0\n") != NULL);   // final sample sees the three queued records

    metrics_thread_unregister();
    metrics_destroy(m);
    bq_destroy(q1);
    bq_destroy(q2);
}

static void test_no_latency_and_write_errors(void)
{
    char prefix[64], line[512];
    metrics_t *m;
    metrics_summary_t s = { 0 };
    bq_t *q = bq_create(1);

    snprintf(prefix, sizeof prefix, "%s/empty", dir);
    m = metrics_create(true, prefix);
    assert(m != NULL && q != NULL);
    assert(metrics_merge(m, &s) == 0);
    assert(!s.have_latency);
    assert(metrics_write_csv(m, &s) == 0);
    read_line(prefix, ".summary.csv", 1, line, sizeof line);
    assert(starts_with(line, "0,0,0.000000,n/a,,,"));   // zero wall time, no samples
    metrics_destroy(m);

    m = metrics_create(true, "/nonexistent/dir/run");
    assert(m != NULL);
    assert(metrics_merge(m, &s) == 0);
    errno = 0;
    assert(metrics_write_csv(m, &s) == -1);
    assert(errno == ENOENT);
    metrics_destroy(m);

    /* No prefix: collected for the summary line, but no monitor and no files. */
    m = metrics_create(true, NULL);
    assert(m != NULL);
    assert(metrics_record_block(m, 0, 3000) == 0);
    assert(metrics_monitor_start(m, NULL, NULL, 0) == 0);
    assert(metrics_monitor_stop(m) == 0);
    assert(metrics_merge(m, &s) == 0);
    assert(s.have_latency && s.lat_p50_ns == 3000);
    assert(metrics_write_csv(m, &s) == 0);
    metrics_destroy(m);
    bq_destroy(q);
}

/* Stop wakes the monitor instead of waiting out its 10 ms sleep, which used to add up to 10 ms to
 * every --metrics wall time. Fastest of 5 tries, so a loaded machine does not fail the test. */
static void test_stop_is_prompt(void)
{
    char prefix[128];
    bq_t *q1 = bq_create(1), *q2 = bq_create(1);
    uint64_t best = UINT64_MAX;

    snprintf(prefix, sizeof prefix, "%s/stop", dir);
    assert(q1 != NULL && q2 != NULL);
    for (int i = 0; i < 5; i++) {
        metrics_t *m = metrics_create(true, prefix);
        uint64_t origin, t0, t1;

        assert(m != NULL);
        assert(metrics_now_ns(&origin) == 0);
        assert(metrics_monitor_start(m, q1, q2, origin) == 0);
        sleep_ms(1);   // the monitor has taken its first sample and is asleep
        assert(metrics_now_ns(&t0) == 0);
        assert(metrics_monitor_stop(m) == 0);
        assert(metrics_now_ns(&t1) == 0);
        if (t1 - t0 < best)
            best = t1 - t0;
        metrics_destroy(m);
    }
    assert(best < 5000000);
    bq_destroy(q1);
    bq_destroy(q2);
}

int main(void)
{
    assert(mkdtemp(dir) != NULL);
    test_now_ns();
    test_disabled_collects_nothing();
    test_enabled_full_run();
    test_no_latency_and_write_errors();
    test_stop_is_prompt();

    const char *files[] = { "run.summary.csv", "run.occupancy.csv", "empty.summary.csv", "empty.occupancy.csv" };
    char path[128];

    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        unlink(path);
    }
    rmdir(dir);
    return 0;
}
