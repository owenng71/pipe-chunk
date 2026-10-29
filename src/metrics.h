#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "queue.h"

typedef struct metrics metrics_t;

typedef struct metrics_summary {
    uint64_t bytes;          /* writer total: len of completed unique output records */
    uint64_t blocks;         /* writer total: completed unique output records */
    uint64_t wall_ns;        /* pipeline CLOCK_MONOTONIC start -> output/manifest fsync */
    bool     have_latency;   /* false means no valid latency values */
    uint64_t lat_p50_ns;
    uint64_t lat_p99_ns;
    bool     have_io_call;   /* false means io_call_s prints n/a */
    uint64_t io_call_ns;
    bool     have_resources; /* false means CPU/RSS fields print n/a */
    double   cpu_user_s;
    double   cpu_sys_s;
    uint64_t peak_rss_kb;
} metrics_summary_t;

/* Writes a CLOCK_MONOTONIC timestamp to caller-owned storage without shared locking; returns 0 or -1 with errno. */
int metrics_now_ns(uint64_t *out_ns);

/* Returns a context owned by the caller; NULL prefix = no CSV files and no occupancy monitor. Returns NULL with errno on failure. */
metrics_t *metrics_create(bool enabled, const char *prefix);

/* Registers caller-thread state owned by m under the registry lock; returns 0 or -1 with the failure in errno. */
int metrics_thread_register(metrics_t *m);

/* Clears only the caller's TLS pointer; m retains the state until run teardown, with no lock or errno result. */
void metrics_thread_unregister(void);

/* Adds elapsed time to context-owned caller-thread TLS state without allocation or locking; NULL TLS is a no-op and no errno is returned. */
void metrics_io_add(uint64_t elapsed_ns);

/* Writer appends latency to m-owned storage without a per-call lock; returns -1/errno for invalid time order, while allocation drops preserve errno and return 0. */
int metrics_record_block(metrics_t *m, uint64_t t_read_ns,
                         uint64_t completion_ns);

/* m owns the monitor and samples, and borrows q1/q2 until stop; lifecycle-synchronized start returns 0 or -1 with errno. */
int metrics_monitor_start(metrics_t *m, bq_t *q1, bq_t *q2,
                          uint64_t run_origin_ns);

/* Stops and joins the monitor using lifecycle synchronization, retaining samples in m; returns 0 or -1 with errno. */
int metrics_monitor_stop(metrics_t *m);

/* After worker joins, merges m-owned state into caller-owned summary without worker locking; returns 0 or -1 with errno. */
int metrics_merge(metrics_t *m, metrics_summary_t *summary);

/* Reads caller-owned summary and m's output configuration without locking after merge; returns 0 or -1 with errno. */
int metrics_write_csv(metrics_t *m, const metrics_summary_t *summary);

/* Reads a borrowed summary and writes stderr without locking; this void reporting call has no errno result. */
void metrics_print_summary(const metrics_summary_t *summary);

/* Caller releases m only after all users stop; destroys owned state without concurrent locking or an errno result. */
void metrics_destroy(metrics_t *m);

#endif
