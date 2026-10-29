/* pipeline.c — --mode=pipeline: reader → Q1 → N workers (SHA-256) → Q2 → writer. */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>

#include "files.h"
#include "io.h"
#include "metrics.h"
#include "pipeline.h"
#include "pool.h"
#include "queue.h"
#include "ratelimit.h"

typedef struct run {
    const config_t *cfg;
    files_t files;          // in: reader only · out, manifest: writer only
    pool_t *pool;
    bq_t *q1, *q2;
    metrics_t *metrics;
    atomic_bool failed;
    /* Writer-owned until it is joined. */
    uint8_t *written;       // bitmap of written seq: a record that arrives twice is dropped
    size_t written_bytes;
    uint64_t bytes, blocks;
} run_t;

/*
 * Stops the run after an error: closing the pool and both queues wakes every blocked stage. Stages
 * then return the records they hold to the pool and exit; the writer drains Q2 without writing.
 */
static void run_fail(run_t *run)
{
    if (atomic_exchange(&run->failed, true))
        return;
    pool_close(run->pool);
    bq_close(run->q1);
    bq_close(run->q2);
}

static void report_thread_error(const char *stage, int err)
{
    fprintf(stderr, "pipe-chunk: %s: %s\n", stage, strerror(err));
}

static void *reader_main(void *arg)
{
    run_t *run = arg;
    const config_t *cfg = run->cfg;
    ratelimit_t rate;
    uint64_t seq = 0;
    off_t off = 0;

    (void)metrics_thread_register(run->metrics);
    ratelimit_init(&rate, cfg->in_rate);
    for (;;) {
        record_t *r = pool_get(run->pool);
        ssize_t n;

        if (r == NULL)
            break;   // pool closed: the run failed
        n = read_full(run->files.in, r->buf, cfg->block_size);
        if (n <= 0) {
            if (n < 0) {
                files_report("read error on", cfg->input);
                run_fail(run);
            }
            pool_put(run->pool, r);
            break;
        }
        ratelimit_wait(&rate, (size_t)n);
        r->seq = seq++;
        r->offset = off;
        r->len = (size_t)n;
        (void)metrics_now_ns(&r->t_read_ns);
        off += n;
        if (bq_push(run->q1, r) < 0) {   // closed: the run failed
            pool_put(run->pool, r);
            break;
        }
        /* r now belongs to Q1 and must not be touched. */
        if ((size_t)n < cfg->block_size)
            break;   // read_full is short only at EOF
    }
    bq_close(run->q1);   // EOF: workers drain Q1 and exit
    metrics_thread_unregister();
    return NULL;
}

static void *worker_main(void *arg)
{
    run_t *run = arg;
    record_t *r;
    int rc;

    (void)metrics_thread_register(run->metrics);
    while ((rc = bq_pop(run->q1, &r)) == 1) {
        SHA256(r->buf, r->len, r->sha);
        if (bq_push(run->q2, r) < 0)
            pool_put(run->pool, r);   // Q2 closed: the run failed
    }
    if (rc < 0) {
        report_thread_error("worker", errno);
        run_fail(run);
    }
    metrics_thread_unregister();
    return NULL;
}

/* Marks seq as written. Returns 1 if it already was, 0 if newly marked, -1 on allocation failure. */
static int mark_written(run_t *run, uint64_t seq)
{
    size_t byte = (size_t)(seq / 8);
    uint8_t bit = (uint8_t)(1u << (seq % 8));

    if (byte >= run->written_bytes) {
        size_t size = run->written_bytes ? run->written_bytes : 1024;
        uint8_t *w;

        while (size <= byte)
            size *= 2;
        w = realloc(run->written, size);
        if (w == NULL)
            return -1;
        memset(w + run->written_bytes, 0, size - run->written_bytes);
        run->written = w;
        run->written_bytes = size;
    }
    if (run->written[byte] & bit)
        return 1;
    run->written[byte] |= bit;
    return 0;
}

static void *writer_main(void *arg)
{
    run_t *run = arg;
    const config_t *cfg = run->cfg;
    ratelimit_t rate;
    record_t *r;
    int rc;

    (void)metrics_thread_register(run->metrics);
    ratelimit_init(&rate, cfg->out_rate);
    while ((rc = bq_pop(run->q2, &r)) == 1) {
        uint64_t now;
        int dup;

        if (atomic_load(&run->failed)) {
            pool_put(run->pool, r);   // draining after an error
            continue;
        }
        dup = mark_written(run, r->seq);
        if (dup != 0) {
            if (dup < 0) {
                report_thread_error("writer", ENOMEM);
                run_fail(run);
            }
            pool_put(run->pool, r);
            continue;
        }
        ratelimit_wait(&rate, r->len);
        if (pwrite_full(run->files.out, r->buf, r->len, r->offset) < 0) {
            files_report("write error on", cfg->output);
            run_fail(run);
        } else if (files_write_manifest(cfg, &run->files, r) < 0) {
            run_fail(run);
        } else {
            (void)metrics_now_ns(&now);
            (void)metrics_record_block(run->metrics, r->t_read_ns, now);
            run->bytes += r->len;
            run->blocks++;
        }
        pool_put(run->pool, r);
    }
    if (rc < 0) {
        report_thread_error("writer", errno);
        run_fail(run);
    }
    metrics_thread_unregister();
    return NULL;
}

int pipeline_run(const config_t *cfg)
{
    run_t run = { .cfg = cfg };
    pthread_t reader, writer, *workers = NULL;
    int nworkers = 0, rc;
    bool reader_started = false, writer_started = false, ok;
    size_t qcap = cfg->unbounded ? 0 : cfg->queue_cap;
    /* Reader, every Q1 slot, every worker, every Q2 slot and the writer can each hold one tray. */
    size_t trays = cfg->unbounded ? 0 : 2 * cfg->queue_cap + (size_t)cfg->workers + 2;
    metrics_summary_t summary = { 0 };
    uint64_t start = 0, end;

    atomic_init(&run.failed, false);
    if (files_open(cfg, &run.files) < 0)
        return 1;
    (void)metrics_now_ns(&start);
    run.metrics = metrics_create(true, cfg->metrics);
    run.pool = pool_create(trays, cfg->block_size);
    run.q1 = bq_create(qcap);
    run.q2 = bq_create(qcap);
    workers = calloc((size_t)cfg->workers, sizeof(*workers));
    if (run.metrics == NULL || run.pool == NULL || run.q1 == NULL || run.q2 == NULL || workers == NULL) {
        files_report("cannot set up pipeline for", cfg->input);
        atomic_store(&run.failed, true);
        goto teardown;
    }
    if (metrics_monitor_start(run.metrics, run.q1, run.q2, start) < 0)
        files_report("cannot start occupancy monitor for", cfg->input);

    /* Start consumers first, so the reader never fills a queue nobody drains. */
    rc = pthread_create(&writer, NULL, writer_main, &run);
    writer_started = rc == 0;
    for (; rc == 0 && nworkers < cfg->workers; nworkers++)
        if ((rc = pthread_create(&workers[nworkers], NULL, worker_main, &run)) != 0)
            break;
    if (rc == 0) {
        rc = pthread_create(&reader, NULL, reader_main, &run);
        reader_started = rc == 0;
    }
    if (rc != 0) {
        report_thread_error("cannot start thread", rc);
        run_fail(&run);   // also closes Q1, which the reader would otherwise close
    }

    /* Shutdown order: reader (closes Q1 at EOF) → workers drain Q1 → close Q2 → writer drains Q2. */
    if (reader_started)
        (void)pthread_join(reader, NULL);
    for (int i = 0; i < nworkers; i++)
        (void)pthread_join(workers[i], NULL);
    bq_close(run.q2);
    if (writer_started)
        (void)pthread_join(writer, NULL);
    (void)metrics_monitor_stop(run.metrics);

teardown:
    ok = files_close(cfg, &run.files, !atomic_load(&run.failed)) == 0;
    if (ok) {
        (void)metrics_now_ns(&end);
        summary.bytes = run.bytes;
        summary.blocks = run.blocks;
        summary.wall_ns = end - start;
        if (metrics_merge(run.metrics, &summary) == 0)
            (void)metrics_write_csv(run.metrics, &summary);
        metrics_print_summary(&summary);
    }
    /* Queues hold only borrowed pointers; destroy them before the pool that owns the trays. */
    bq_destroy(run.q1);
    bq_destroy(run.q2);
    pool_destroy(run.pool);
    metrics_destroy(run.metrics);
    free(workers);
    free(run.written);
    return ok ? 0 : 1;
}
