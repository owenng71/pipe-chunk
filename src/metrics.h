# Proposed Contract: `metrics.h`

**Task ID**: W2-06
**Status**: Draft proposal; not a frozen Overview section 10 interface. The review PR carries a
draft `src/metrics.h`; Phat and Vy review it before `src/metrics.c` implementation and
`tests/test_metrics.c` work. The reviewed header is finalized after the gate.

This contract describes the intended public API and ownership. Exact names/signatures are subject
to that review. It does not authorize a change to `record_t`, `common.h`, `io.h`, or `io.c`.

## Proposed Types and Functions

```c
#ifndef METRICS_H
#define METRICS_H

#include <stdbool.h>
#include <stdint.h>

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

int metrics_now_ns(uint64_t *out_ns);
metrics_t *metrics_create(bool enabled, const char *prefix);
int metrics_thread_register(metrics_t *m);
void metrics_thread_unregister(void);
void metrics_io_add(uint64_t elapsed_ns);
int metrics_record_block(metrics_t *m, uint64_t t_read_ns,
                         uint64_t completion_ns);
int metrics_monitor_start(metrics_t *m, bq_t *q1, bq_t *q2,
                          uint64_t run_origin_ns);
int metrics_monitor_stop(metrics_t *m);
int metrics_merge(metrics_t *m, metrics_summary_t *summary);
int metrics_write_csv(metrics_t *m, const metrics_summary_t *summary);
void metrics_print_summary(const metrics_summary_t *summary);
void metrics_destroy(metrics_t *m);

#endif
```

## Contract Details

- Status functions return `0` or `-1` and preserve the failure cause in `errno`. `metrics_create`
  returns `NULL` with `errno` on failure. `metrics_thread_unregister` and `metrics_io_add` are
  no-ops when the calling thread has no registered enabled state. POSIX pthread calls return an
  error number directly; the module copies that value to `errno` before returning failure.
- `metrics_now_ns` returns a `CLOCK_MONOTONIC` timestamp. The caller uses it for `record_t.t_read_ns`
  and writer completion timestamps so both use the same clock source.
- `metrics_thread_register` is called once at the start of each participating thread. It installs
  a stable state in the run registry under the only metrics registry lock, then assigns that state
  to a `_Thread_local` pointer. A thread may have only one active registration at once. For a
  disabled context it returns success without allocating or setting TLS.
- `metrics_thread_unregister` only clears the current thread's TLS pointer; it does not acquire the
  registry lock or free state. The run owns registered states through all `pthread_join` calls.
- `metrics_io_add(ns)` accumulates only in the calling thread's TLS state. If the TLS pointer is
  `NULL`, it returns immediately as a safe no-op. Otherwise it does no allocation, locking, or I/O.
  In disabled mode registration is not performed, so the call is a no-op.
- Vy's separate `io.c` hook measures elapsed `CLOCK_MONOTONIC` time around each individual
  `read`, `write`, or `poll` call and passes the difference to `metrics_io_add`. Repeated syscall or
  poll calls are counted separately; time between calls is excluded. This plan does not edit
  `src/io.c`.
- The metrics module reports process I/O-call elapsed time through `io_call_s`; it does not collect
  or output system-wide `vmstat`/`pidstat` iowait, has no summary CSV column for it, and does not
  test it. System-wide iowait is collected separately and labeled separately if reported.
- Only the writer calls `metrics_record_block`. It appends `completion_ns - t_read_ns` to the
  writer-owned chunked latency store. A timestamp inversion fails with `EINVAL`. If allocating a
  chunk fails, the current and all subsequent unstored samples are dropped and counted, latency
  collection is disabled for the rest of the run, and `metrics_record_block` still returns success
  to the writer while preserving its `errno`; this must not become a record or copy failure. At
  final merge, emit exactly one stderr warning with the total dropped-sample count and set
  `have_latency` false. Both percentiles are `n/a` in the stderr summary and CSV because a partial
  set is invalid. In disabled mode `metrics_record_block` is a no-op.
- `metrics_monitor_start` is a no-op when disabled. When enabled it starts one monitor thread, which
  samples immediately, then on absolute `CLOCK_MONOTONIC` deadlines every 10 ms, and at stop. It
  uses `clock_nanosleep(..., TIMER_ABSTIME, ...)`; an `EINTR` retry uses the same deadline. Stop
  atomically requests shutdown and joins the thread; it may wait up to one sampling interval. If
  the monitor wakes late, it records one current sample and skips expired deadlines rather than
  generating catch-up rows. If a periodic sample coincides with stop, that row is the final sample
  rather than adding a duplicate.
- Each occupancy row pairs `bq_size(q1)` and `bq_size(q2)` calls with one sample timestamp. The two
  sizes are individually synchronized but not a single atomic snapshot.
- `metrics_merge` is called only after `pthread_join` for all registered threads and after monitor
  stop/join. It sums thread I/O nanoseconds, sorts latency samples, and fills the metrics-specific
  `metrics_summary_t` fields. The caller supplies the always-on totals: the writer increments
  `bytes` by `record_t.len` and `blocks` by one after each unique record completes output and
  manifest bookkeeping; the Phat-owned pipeline controller supplies `wall_ns` from its monotonic
  timestamp immediately before starting pipeline stages through output/manifest `fsync`, before
  metrics CSV writing. Providing
  these values and invoking the metrics lifecycle/output APIs is a W2-01 Phat pipeline integration
  dependency; the
  module unit tests supply synthetic totals directly.
  It also calls `getrusage(RUSAGE_SELF)` once to fill the CPU/RSS fields for both enabled and
  disabled runs. For `N > 0`, nearest-rank p50/p99 use integer index
  `(pct * N + 99) / 100 - 1` with `pct` equal to 50 or 99. Empty latency input sets
  `have_latency` false; disabled mode sets latency and I/O availability flags false.
- `metrics_write_csv` writes no files when disabled. Enabled output is exactly one summary header
  and data row with `bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s`,
  plus occupancy header `t_ms,q1_size,q2_size` and all captured samples. Empty latency samples
  produce empty CSV percentile cells. A latency allocation failure makes both percentile cells
  `n/a`; the stderr summary also uses `n/a` when a latency value is unavailable. Zero wall duration
  produces `n/a` throughput. It uses `fopen`/`fprintf`;
  any open/write/close failure warns on stderr, returns `-1` with the cause in `errno`, and must not
  change the copy exit code.
- CSV units are seconds for `wall_s`, CPU fields, and `io_call_s`; decimal MB/s for throughput
  (`bytes / wall_s / 1,000,000`); microseconds for latency fields; KiB for peak RSS; milliseconds
  for occupancy time; and records for queue sizes.
- `metrics_print_summary` prints one normal one-line summary to stderr on every run. It always uses
  caller-supplied byte/block/wall totals and CPU/RSS values captured by `metrics_merge`. With
  metrics disabled or unavailable, it prints `n/a` for latency percentiles and I/O-call time. If
  `getrusage` failed, CPU/RSS fields are `n/a`. Linux `ru_maxrss` is reported in KiB.
- Metrics collection is optional and does not change output bytes or accepted-record semantics.
  The Phat-owned pipeline lifecycle calls provide start/origin, writer totals, registration,
  merge, output, and final summary. Those call sites are outside this metrics-module plan. The
  separate Vy-owned `io.c` hook requires her approval and a `docs/design.md` entry.

## Review Gate

The review vehicle is a PR containing a draft `src/metrics.h` that matches this proposal and a
`docs/design.md` entry recording the proposed interface and rationale. Phat and Vy review the PR.
The first gate task verifies the PR is merged and the design entry is present before any
`src/metrics.c` implementation or `tests/test_metrics.c` work starts. Any changes to an already
agreed header (`io.h`, `common.h`, or section 10 interfaces) still need an all-three-approved PR and
a `docs/design.md` entry. Spec Kit artifacts remain local and do not create or publish GitHub work.
