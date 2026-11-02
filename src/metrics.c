/* metrics.c — latency, queue occupancy, CPU/memory and I/O-call time; CSV and one-line summary. */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "metrics.h"

enum { SAMPLE_PERIOD_NS = 10000000 };   // occupancy sampled every 10 ms

typedef struct thread_state {
    uint64_t io_ns;                     // written only by its own thread
    struct thread_state *next;
} thread_state_t;

typedef struct sample {
    uint64_t t_ms;
    size_t q1, q2;
} sample_t;

struct metrics {
    bool enabled;
    char *prefix;                       // NULL: collect for the summary line, write no CSV

    pthread_mutex_t registry_mutex;     // guards `threads` (the list, not the counters)
    thread_state_t *threads;

    uint64_t *lat;                      // writer-owned latency samples (ns)
    size_t lat_n, lat_cap;
    bool lat_failed;                    // an allocation failed: the set is incomplete
    uint64_t lat_dropped;

    pthread_t monitor;
    bool monitor_running;
    pthread_mutex_t monitor_mutex;      // guards monitor_stop; exists only while the monitor runs
    pthread_cond_t monitor_wake;        // stop signals it so the monitor does not finish its 10 ms sleep
    bool monitor_stop;
    bq_t *q1, *q2;
    uint64_t origin_ns;
    sample_t *samples;                  // monitor-owned until it is joined
    size_t nsamples, samples_cap;
};

static _Thread_local thread_state_t *tls_state;

int metrics_now_ns(uint64_t *out_ns)
{
    struct timespec ts;

    if (out_ns == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return -1;
    *out_ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    return 0;
}

metrics_t *metrics_create(bool enabled, const char *prefix)
{
    int saved_errno = errno;
    metrics_t *m;
    int rc;

    m = calloc(1, sizeof(*m));
    if (m == NULL)
        return NULL;
    m->enabled = enabled;
    if (enabled && prefix != NULL) {
        m->prefix = strdup(prefix);
        if (m->prefix == NULL) {
            free(m);
            errno = ENOMEM;
            return NULL;
        }
    }
    rc = pthread_mutex_init(&m->registry_mutex, NULL);
    if (rc != 0) {
        free(m->prefix);
        free(m);
        errno = rc;
        return NULL;
    }
    errno = saved_errno;
    return m;
}

int metrics_thread_register(metrics_t *m)
{
    int saved_errno = errno;
    thread_state_t *state;
    int rc;

    if (m == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!m->enabled)
        return 0;
    if (tls_state != NULL) {
        errno = EBUSY;   // one active registration per thread
        return -1;
    }
    state = calloc(1, sizeof(*state));
    if (state == NULL)
        return -1;
    rc = pthread_mutex_lock(&m->registry_mutex);
    if (rc != 0) {
        free(state);
        errno = rc;
        return -1;
    }
    state->next = m->threads;
    m->threads = state;
    (void)pthread_mutex_unlock(&m->registry_mutex);
    tls_state = state;
    errno = saved_errno;
    return 0;
}

void metrics_thread_unregister(void)
{
    tls_state = NULL;
}

void metrics_io_add(uint64_t elapsed_ns)
{
    if (tls_state != NULL)
        tls_state->io_ns += elapsed_ns;
}

int metrics_record_block(metrics_t *m, uint64_t t_read_ns, uint64_t completion_ns)
{
    if (m == NULL || completion_ns < t_read_ns) {
        errno = EINVAL;
        return -1;
    }
    if (!m->enabled)
        return 0;
    if (m->lat_failed) {
        m->lat_dropped++;
        return 0;
    }
    if (m->lat_n == m->lat_cap) {
        int saved_errno = errno;
        size_t cap = m->lat_cap ? 2 * m->lat_cap : 4096;
        uint64_t *lat = cap <= SIZE_MAX / sizeof(*lat) ? realloc(m->lat, cap * sizeof(*lat)) : NULL;

        errno = saved_errno;   // running out of memory here must not fail the copy
        if (lat == NULL) {
            m->lat_failed = true;
            m->lat_dropped++;
            return 0;
        }
        m->lat = lat;
        m->lat_cap = cap;
    }
    m->lat[m->lat_n++] = completion_ns - t_read_ns;
    return 0;
}

static void monitor_sample(metrics_t *m)
{
    uint64_t now;

    if (m->nsamples == m->samples_cap) {
        size_t cap = m->samples_cap ? 2 * m->samples_cap : 1024;
        sample_t *s = realloc(m->samples, cap * sizeof(*s));

        if (s == NULL)
            return;   // limit: on allocation failure the occupancy series just stops growing
        m->samples = s;
        m->samples_cap = cap;
    }
    if (metrics_now_ns(&now) < 0)
        return;
    m->samples[m->nsamples++] = (sample_t){ (now - m->origin_ns) / 1000000u, bq_size(m->q1), bq_size(m->q2) };
}

static void *monitor_main(void *arg)
{
    metrics_t *m = arg;
    uint64_t next, now;
    bool stop = false;

    if (metrics_now_ns(&next) < 0)
        return NULL;
    for (;;) {
        monitor_sample(m);   // the sample taken after stop is requested is the final one
        if (stop)
            break;
        next += SAMPLE_PERIOD_NS;
        if (metrics_now_ns(&now) == 0 && now > next)
            next = now;   // woke late: skip missed deadlines instead of catching up
        struct timespec until = { (time_t)(next / 1000000000u), (long)(next % 1000000000u) };
        int rc = 0;

        pthread_mutex_lock(&m->monitor_mutex);
        while (!m->monitor_stop && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&m->monitor_wake, &m->monitor_mutex, &until);
        stop = m->monitor_stop;
        pthread_mutex_unlock(&m->monitor_mutex);
    }
    return NULL;
}

int metrics_monitor_start(metrics_t *m, bq_t *q1, bq_t *q2, uint64_t run_origin_ns)
{
    int rc;

    if (m == NULL || (m->enabled && m->prefix != NULL && (q1 == NULL || q2 == NULL)) || m->monitor_running) {
        errno = EINVAL;
        return -1;
    }
    if (!m->enabled || m->prefix == NULL)
        return 0;   // samples are only written to the occupancy CSV
    m->q1 = q1;
    m->q2 = q2;
    m->origin_ns = run_origin_ns;
    m->monitor_stop = false;

    pthread_condattr_t attr;

    rc = pthread_condattr_init(&attr);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);   // deadlines come from metrics_now_ns
    if (rc == 0)
        rc = pthread_cond_init(&m->monitor_wake, &attr);
    pthread_condattr_destroy(&attr);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    rc = pthread_mutex_init(&m->monitor_mutex, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&m->monitor_wake);
        errno = rc;
        return -1;
    }
    rc = pthread_create(&m->monitor, NULL, monitor_main, m);
    if (rc != 0) {
        pthread_mutex_destroy(&m->monitor_mutex);
        pthread_cond_destroy(&m->monitor_wake);
        errno = rc;
        return -1;
    }
    m->monitor_running = true;
    return 0;
}

int metrics_monitor_stop(metrics_t *m)
{
    int rc;

    if (m == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!m->monitor_running)
        return 0;
    pthread_mutex_lock(&m->monitor_mutex);
    m->monitor_stop = true;
    pthread_cond_signal(&m->monitor_wake);
    pthread_mutex_unlock(&m->monitor_mutex);
    rc = pthread_join(m->monitor, NULL);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    pthread_mutex_destroy(&m->monitor_mutex);
    pthread_cond_destroy(&m->monitor_wake);
    m->monitor_running = false;
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

/* Nearest-rank percentile of n sorted samples, n > 0. */
static uint64_t percentile(const uint64_t *sorted, size_t n, unsigned pct)
{
    return sorted[(pct * n + 99) / 100 - 1];
}

int metrics_merge(metrics_t *m, metrics_summary_t *summary)
{
    struct rusage ru;

    if (m == NULL || summary == NULL || m->monitor_running) {
        errno = EINVAL;
        return -1;
    }

    summary->have_io_call = m->enabled;
    summary->io_call_ns = 0;
    for (thread_state_t *t = m->threads; t != NULL; t = t->next)
        summary->io_call_ns += t->io_ns;

    summary->have_latency = m->enabled && !m->lat_failed && m->lat_n > 0;
    summary->lat_p50_ns = summary->lat_p99_ns = 0;
    if (m->lat_failed)
        fprintf(stderr, "pipe-chunk: metrics: out of memory, %" PRIu64 " latency samples dropped; "
                "latency not reported\n", m->lat_dropped);
    if (summary->have_latency) {
        qsort(m->lat, m->lat_n, sizeof(*m->lat), cmp_u64);
        summary->lat_p50_ns = percentile(m->lat, m->lat_n, 50);
        summary->lat_p99_ns = percentile(m->lat, m->lat_n, 99);
    }

    summary->have_resources = getrusage(RUSAGE_SELF, &ru) == 0;
    if (summary->have_resources) {
        summary->cpu_user_s = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
        summary->cpu_sys_s = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
        summary->peak_rss_kb = (uint64_t)ru.ru_maxrss;   // KiB on Linux
    }
    return 0;
}

/* Formats a latency cell: empty for no samples, n/a when samples were dropped. */
static const char *lat_cell(const metrics_t *m, const metrics_summary_t *s, uint64_t ns, char *buf, size_t size)
{
    if (m->lat_failed)
        return "n/a";
    if (!s->have_latency)
        return "";
    snprintf(buf, size, "%.1f", (double)ns / 1e3);
    return buf;
}

static int write_file(const char *prefix, const char *suffix, FILE **f)
{
    char path[4096];

    if ((size_t)snprintf(path, sizeof path, "%s%s", prefix, suffix) >= sizeof path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    *f = fopen(path, "w");
    return *f == NULL ? -1 : 0;
}

int metrics_write_csv(metrics_t *m, const metrics_summary_t *s)
{
    char p50[32], p99[32], tput[32] = "n/a", io[32] = "n/a", cpu_u[32] = "n/a", cpu_s[32] = "n/a", rss[32] = "n/a";
    double wall_s;
    FILE *f;
    int ok;

    if (m == NULL || s == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!m->enabled || m->prefix == NULL)
        return 0;

    wall_s = (double)s->wall_ns / 1e9;
    if (s->wall_ns)
        snprintf(tput, sizeof tput, "%.3f", (double)s->bytes / wall_s / 1e6);
    if (s->have_io_call)
        snprintf(io, sizeof io, "%.6f", (double)s->io_call_ns / 1e9);
    if (s->have_resources) {
        snprintf(cpu_u, sizeof cpu_u, "%.6f", s->cpu_user_s);
        snprintf(cpu_s, sizeof cpu_s, "%.6f", s->cpu_sys_s);
        snprintf(rss, sizeof rss, "%" PRIu64, s->peak_rss_kb);
    }

    if (write_file(m->prefix, ".summary.csv", &f) < 0)
        goto fail;
    ok = fprintf(f, "bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,"
                 "cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s\n") >= 0;
    ok = ok && fprintf(f, "%" PRIu64 ",%" PRIu64 ",%.6f,%s,%s,%s,%s,%s,%s,%s\n", s->bytes, s->blocks, wall_s,
                       tput, lat_cell(m, s, s->lat_p50_ns, p50, sizeof p50),
                       lat_cell(m, s, s->lat_p99_ns, p99, sizeof p99), cpu_u, cpu_s, rss, io) >= 0;
    if (fclose(f) != 0 || !ok)
        goto fail;

    if (write_file(m->prefix, ".occupancy.csv", &f) < 0)
        goto fail;
    ok = fprintf(f, "t_ms,q1_size,q2_size\n") >= 0;
    for (size_t i = 0; ok && i < m->nsamples; i++)
        ok = fprintf(f, "%" PRIu64 ",%zu,%zu\n", m->samples[i].t_ms, m->samples[i].q1, m->samples[i].q2) >= 0;
    if (fclose(f) != 0 || !ok)
        goto fail;
    return 0;

fail:
    {
        int err = errno ? errno : EIO;

        fprintf(stderr, "pipe-chunk: metrics: cannot write %s CSV: %s\n", m->prefix, strerror(err));
        errno = err;
        return -1;
    }
}

void metrics_print_summary(const metrics_summary_t *s)
{
    char p50[32] = "n/a", p99[32] = "n/a", tput[32] = "n/a", io[32] = "n/a", cpu[64] = "n/a", rss[32] = "n/a";

    if (s == NULL)
        return;
    if (s->wall_ns)
        snprintf(tput, sizeof tput, "%.1f MB/s", (double)s->bytes / ((double)s->wall_ns / 1e9) / 1e6);
    if (s->have_latency) {
        snprintf(p50, sizeof p50, "%.0f us", (double)s->lat_p50_ns / 1e3);
        snprintf(p99, sizeof p99, "%.0f us", (double)s->lat_p99_ns / 1e3);
    }
    if (s->have_io_call)
        snprintf(io, sizeof io, "%.3f s", (double)s->io_call_ns / 1e9);
    if (s->have_resources) {
        snprintf(cpu, sizeof cpu, "user %.3f s, sys %.3f s", s->cpu_user_s, s->cpu_sys_s);
        snprintf(rss, sizeof rss, "%" PRIu64 " KiB", s->peak_rss_kb);
    }
    fprintf(stderr, "pipe-chunk: %" PRIu64 " bytes in %" PRIu64 " blocks, %.3f s, %s; latency p50 %s, p99 %s; "
            "cpu %s; peak RSS %s; I/O calls %s\n", s->bytes, s->blocks, (double)s->wall_ns / 1e9, tput,
            p50, p99, cpu, rss, io);
}

void metrics_destroy(metrics_t *m)
{
    thread_state_t *t, *next;

    if (m == NULL)
        return;
    (void)metrics_monitor_stop(m);
    for (t = m->threads; t != NULL; t = next) {
        next = t->next;
        free(t);
    }
    (void)pthread_mutex_destroy(&m->registry_mutex);
    free(m->samples);
    free(m->lat);
    free(m->prefix);
    free(m);
}
