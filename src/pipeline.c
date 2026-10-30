/* pipeline.c — --mode=pipeline: reader → Q1 → N workers (SHA-256) → Q2 → writer. */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>

#include "cancel.h"
#include "faults.h"
#include "files.h"
#include "io.h"
#include "metrics.h"
#include "pipeline.h"
#include "pool.h"
#include "queue.h"
#include "ratelimit.h"
#include "seqset.h"

typedef struct run {
    const config_t *cfg;
    files_t files;          // in: reader only · out, manifest: writer only
    pool_t *pool;
    bq_t *q1, *q2;
    metrics_t *metrics;
    cancel_t *cancel;
    atomic_bool failed;
    pthread_mutex_t sup_mutex;   // guards slot_t.exited
    pthread_cond_t sup_cond;     // a worker exited
    /* Writer-owned until it is joined. */
    seqset_t written;       // seq already written: a record that arrives twice is dropped
    uint64_t bytes, blocks;
} run_t;

/*
 * One worker. While its thread runs, the worker owns in_flight and rng; the supervisor reads them
 * only after pthread_join, which orders the accesses.
 */
typedef struct slot {
    run_t *run;
    pthread_t thread;
    record_t *in_flight;    // record being hashed; still set if the worker died holding it
    unsigned rng;           // rand_r state for --fail-worker
    bool exited;            // under run->sup_mutex
} slot_t;

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

/*
 * First Ctrl+C (runs in the signal thread): no new records are accepted. Closing Q1 makes the reader's
 * next push fail; workers and the writer still finish every record already in Q1 or later.
 * limit: a reader blocked on an idle stdin pipe sees the cancel only when data or EOF arrives.
 */
static void run_drain(void *arg)
{
    run_t *run = arg;

    bq_close(run->q1);
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
        n = files_read(cfg, &run->files, r, off, cfg->block_size);
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
        r->attempts = 0;
        r->offset = off;
        r->len = (size_t)n;
        (void)metrics_now_ns(&r->t_read_ns);
        off += n;
        if (bq_push(run->q1, r) < 0) {   // closed: the run failed or was cancelled
            pool_put(run->pool, r);
            break;
        }
        /* r now belongs to Q1 and must not be touched. */
        if ((size_t)n < cfg->block_size)
            break;   // files_read is short only at EOF
    }
    bq_close(run->q1);   // EOF or cancel: workers drain Q1 and exit
    metrics_thread_unregister();
    return NULL;
}

/* --fail-worker: true with probability P. */
static bool worker_dies(slot_t *s)
{
    double p = s->run->cfg->fail_worker;

    return p > 0 && rand_r(&s->rng) < p * ((double)RAND_MAX + 1);
}

/* Starts with the slot's in_flight record if the supervisor handed one over (a retry), then pops Q1. */
static void *worker_main(void *arg)
{
    slot_t *s = arg;
    run_t *run = s->run;
    record_t *r = s->in_flight;
    int rc = 0;

    (void)metrics_thread_register(run->metrics);
    while (r != NULL || (rc = bq_pop(run->q1, &r)) == 1) {
        s->in_flight = r;
        r->attempts++;
        SHA256(r->buf, r->len, r->sha);
        if (worker_dies(s))
            goto out;   // injected crash: exit holding r; the supervisor retries it
        s->in_flight = NULL;
        if (bq_push(run->q2, r) < 0)
            pool_put(run->pool, r);   // Q2 closed: the run failed
        r = NULL;
    }
    if (rc < 0) {
        report_thread_error("worker", errno);
        run_fail(run);
    }
out:
    metrics_thread_unregister();
    pthread_mutex_lock(&run->sup_mutex);
    s->exited = true;
    pthread_cond_signal(&run->sup_cond);
    pthread_mutex_unlock(&run->sup_mutex);
    return NULL;
}

/*
 * A worker died holding s->in_flight. Its replacement gets the record directly instead of through
 * Q1, which the reader or a Ctrl+C may already have closed. Returns true if the replacement runs.
 */
static bool worker_restart(run_t *run, slot_t *s)
{
    record_t *r = s->in_flight;
    int rc;

    if (r->attempts >= MAX_ATTEMPTS) {
        fprintf(stderr, "pipe-chunk: block %" PRIu64 ": worker died %d times, giving up\n", r->seq, r->attempts);
    } else {
        rc = pthread_create(&s->thread, NULL, worker_main, s);
        if (rc == 0)
            return true;
        report_thread_error("cannot restart worker", rc);
    }
    s->in_flight = NULL;
    pool_put(run->pool, r);
    run_fail(run);
    return false;
}

/* Joins workers as they exit and restarts the ones that died, until none is left. */
static void supervise(run_t *run, slot_t *slots, int nworkers)
{
    int live = nworkers;

    pthread_mutex_lock(&run->sup_mutex);
    while (live > 0) {
        slot_t *s = NULL;

        for (int i = 0; i < nworkers && s == NULL; i++)
            if (slots[i].exited)
                s = &slots[i];
        if (s == NULL) {
            pthread_cond_wait(&run->sup_cond, &run->sup_mutex);
            continue;
        }
        s->exited = false;
        pthread_mutex_unlock(&run->sup_mutex);
        (void)pthread_join(s->thread, NULL);
        if (s->in_flight == NULL || !worker_restart(run, s))
            live--;
        pthread_mutex_lock(&run->sup_mutex);
    }
    pthread_mutex_unlock(&run->sup_mutex);
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
        dup = seqset_add(&run->written, r->seq);
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
    run_t run = { .cfg = cfg, .sup_mutex = PTHREAD_MUTEX_INITIALIZER, .sup_cond = PTHREAD_COND_INITIALIZER };
    pthread_t reader, writer;
    slot_t *workers = NULL;
    int nworkers = 0, rc;
    bool reader_started = false, writer_started = false, interrupted = false, ok;
    size_t qcap = cfg->unbounded ? 0 : cfg->queue_cap;
    /* Reader, every Q1 slot, every worker, every Q2 slot and the writer can each hold one tray. */
    size_t trays = cfg->unbounded ? 0 : 2 * cfg->queue_cap + (size_t)cfg->workers + 2;
    metrics_summary_t summary = { 0 };
    uint64_t start = 0, end;

    atomic_init(&run.failed, false);
    faults_init(cfg);
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
    /* Before any other thread starts, so every thread inherits the blocked SIGINT/SIGTERM. */
    run.cancel = cancel_start(run_drain, &run, run.files.manifest);
    if (run.cancel == NULL) {
        files_report("cannot start signal thread for", cfg->input);
        atomic_store(&run.failed, true);
        goto teardown;
    }
    if (metrics_monitor_start(run.metrics, run.q1, run.q2, start) < 0)
        files_report("cannot start occupancy monitor for", cfg->input);

    /* Start consumers first, so the reader never fills a queue nobody drains. */
    rc = pthread_create(&writer, NULL, writer_main, &run);
    writer_started = rc == 0;
    for (; rc == 0 && nworkers < cfg->workers; nworkers++) {
        workers[nworkers].run = &run;
        workers[nworkers].rng = (unsigned)cfg->seed + (unsigned)nworkers * 7919u;
        if ((rc = pthread_create(&workers[nworkers].thread, NULL, worker_main, &workers[nworkers])) != 0)
            break;
    }
    if (rc == 0) {
        rc = pthread_create(&reader, NULL, reader_main, &run);
        reader_started = rc == 0;
    }
    if (rc != 0) {
        report_thread_error("cannot start thread", rc);
        run_fail(&run);   // also closes Q1, which the reader would otherwise close
    }

    /*
     * Shutdown order: reader (closes Q1 at EOF) → workers drain Q1 → close Q2 → writer drains Q2.
     * Supervise before joining the reader: if every worker died while the reader waits on a full Q1,
     * only a restart lets it finish. Workers exit for good only once Q1 is closed.
     */
    supervise(&run, workers, nworkers);
    if (reader_started)
        (void)pthread_join(reader, NULL);
    bq_close(run.q2);
    if (writer_started)
        (void)pthread_join(writer, NULL);
    (void)metrics_monitor_stop(run.metrics);

teardown:
    /* Stop the signal thread before the manifest it may flush is closed. */
    interrupted = cancel_requested(run.cancel);
    cancel_stop(run.cancel);
    ok = files_close(cfg, &run.files, !atomic_load(&run.failed)) == 0;
    if (ok && interrupted)
        fprintf(stderr, "pipe-chunk: interrupted: %" PRIu64 " blocks written and listed in the manifest\n", run.blocks);
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
    seqset_free(&run.written);
    pthread_cond_destroy(&run.sup_cond);
    pthread_mutex_destroy(&run.sup_mutex);
    return !ok ? 1 : interrupted ? 130 : 0;
}
