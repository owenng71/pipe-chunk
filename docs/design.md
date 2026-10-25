# Design Change Log

Record behavior and interface changes here. Changes to `record_t`, `src/common.h`, or any agreed
header interface require a pull request approved by all three team members and a corresponding entry
in this file. Add the task ID, decision, affected interfaces, and rationale for each change.

No new interface or behavior changes are recorded by establishing this log.

## W1-08 — `bq_pop()` return-value clarification

- **Decision**: Clarify Overview section 10 to state the existing `src/queue.h` meanings explicitly:
  `1` when an item is returned and `0` when the queue is closed and empty. Under Constitution III,
  these frozen section 10 values take precedence over the generic status-return convention; a wait
  failure returns `-1` with its cause in `errno`.
- **Confirmation**: Phat confirmed these `bq_pop()` return values on 2026-10-24.
- **Rationale**: Keep the overview's shared contract consistent with the already documented queue
  header and remove the ambiguity around `bq_pop()`'s item-success value.
- **Approval**: Approved by Phat and Vy in PR #<1>, merged <25.10.2026>.
- **Status**: The required shared-contract review is complete and recorded.

## W1-09 — `pool_destroy` interface proposal

- **Decision**: Add `void pool_destroy(pool_t *p);` to Overview section 10. It frees every tray
  and each original buffer from the pool master list, including checked-out trays; it never frees a
  repointed `buf`. Destroy queues before the pool.
- **Affected interface**: `pool_destroy(pool_t *p)` in the pool interface.
- **Rationale**: Reclaim all pool-owned memory at shutdown while preserving ownership of any
  external buffer address currently stored in a tray's `buf` field. Destroying queues first removes
  queued record references before the pool frees tray storage.
- **Approval**: Approved by Phat and Vy in PR #<1> (W1-08 and section 10 updates), merged <25.10.2026>; the PR added `void pool_destroy(pool_t *p);` to Overview section 10.
- **Status**: Approved section 10 interface

## W2-06 — Pipeline metrics interface proposal

- **Status**: proposed, pending Phat and Vy re-review. `src/metrics.h` is a draft proposal, not a
  frozen Overview section 10 interface. The declarations are subject to review and do not approve
  changes to `record_t`, `common.h`, `io.h`, or `io.c`.
- **Affected interface**: The metrics types and declarations in `src/metrics.h`.
- **Decision**: Keep optional pipeline metrics in a standalone module. Use monotonic timestamps,
  per-thread I/O accumulators reached through TLS, writer-only latency collection, post-join merge,
  an occupancy monitor, CSV output, and an always-on one-line summary. Metrics must not change
  output bytes or accepted-record semantics.
- **Rationale**: This keeps I/O timing free of a shared per-call lock, preserves writer and pipeline
  ownership of their existing totals, and lets tests provide synthetic timestamps and totals without
  modifying shared record or I/O interfaces.
- **Status and error handling**: Status-returning functions return `0` or `-1` and preserve the
  failure cause in `errno`; `metrics_create` returns `NULL` with `errno` on failure. POSIX pthread
  functions return their error number directly, which the module copies to `errno` before reporting
  failure. `metrics_thread_unregister` and `metrics_io_add` are no-ops without registered enabled
  thread state.
- **Timestamps and thread state**: `metrics_now_ns` returns a `CLOCK_MONOTONIC` timestamp used for
  both `record_t.t_read_ns` and writer completion timestamps. Each participating thread registers
  once at thread start. Registration adds a stable state to the run registry under the only metrics
  registry lock, then sets a `_Thread_local` pointer; a thread can have one active registration.
  Disabled registration succeeds without allocating or setting TLS. Unregister clears only the
  calling thread's TLS pointer; it does not lock or free the registry state. The run owns all states
  until worker threads have joined.
- **I/O-call timing**: `metrics_io_add` accumulates only in the calling thread's TLS state. A NULL
  pointer is a safe no-op; otherwise the call performs no allocation, locking, or I/O. Vy's separate
  `io.c` hook measures elapsed `CLOCK_MONOTONIC` time around each individual `read`, `write`, or
  `poll` call and passes the difference to `metrics_io_add`. Calls are counted separately and time
  between calls is excluded. The metrics module reports process I/O-call elapsed time as
  `io_call_s`; it does not collect system-wide `vmstat`/`pidstat` iowait, add a summary column for
  it, or test it. System-wide iowait is collected and labeled separately if reported.
- **Latency collection**: Only the writer calls `metrics_record_block`. It appends
  `completion_ns - t_read_ns` to the writer-owned chunked latency store. Timestamp inversion fails
  with `EINVAL`. If a chunk allocation fails, the current and later unstored samples are dropped and
  counted, and latency collection stops for the rest of the run. The call still succeeds for the
  writer while preserving `errno`; allocation failure must not become a record or copy failure.
  Final merge emits exactly one stderr warning with the drop count and clears `have_latency`; both
  percentile fields are `n/a` in the summary and CSV because a partial set is invalid. Disabled
  `metrics_record_block` is a no-op.
- **Occupancy monitor**: `metrics_monitor_start` is a no-op when disabled. When enabled, one monitor
  thread samples immediately, then every 10 ms using absolute `CLOCK_MONOTONIC` deadlines through
  `clock_nanosleep(..., TIMER_ABSTIME, ...)`, and samples at stop. An `EINTR` retries the same
  deadline. Stop atomically requests shutdown and joins the thread; it may wait up to one sampling
  interval. A late wake records one current sample and skips expired deadlines instead of adding
  catch-up rows. If a periodic sample coincides with stop, it is the final sample without a
  duplicate. Each occupancy row pairs `bq_size(q1)` and `bq_size(q2)` with one timestamp; each size
  is synchronized separately, so the pair is not one atomic snapshot.
- **Merge and summary inputs**: `metrics_merge` runs only after every registered thread has joined
  and the monitor has stopped and joined. It sums per-thread I/O nanoseconds, sorts latency samples,
  and fills `metrics_summary_t`. The writer increments always-on `bytes` by `record_t.len` and
  `blocks` by one after each unique record completes output and manifest bookkeeping. The Phat-owned
  pipeline controller supplies `wall_ns` from its monotonic start immediately before pipeline
  stages through output/manifest `fsync`, before metrics CSV writing. Supplying these totals and
  invoking lifecycle/output APIs is a W2-01 Phat pipeline integration dependency; module tests
  supply synthetic totals directly. Merge calls `getrusage(RUSAGE_SELF)` once for CPU/RSS values in
  enabled and disabled modes. For `N > 0`, nearest-rank p50/p99 use integer index
  `(pct * N + 99) / 100 - 1`, where `pct` is 50 or 99. Empty latency input clears `have_latency`;
  disabled mode clears latency and I/O availability flags.
- **CSV output**: Disabled mode writes no files. Enabled output is one summary header and data row
  with these columns, plus occupancy header `t_ms,q1_size,q2_size` and all captured samples:

  ```text
  bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s
  ```
  Empty latency samples produce empty percentile cells; a latency allocation failure produces
  `n/a` for both percentiles. Zero wall duration produces `n/a` throughput. Output uses
  `fopen`/`fprintf`; open, write, or close failures warn to stderr and return `-1` with the cause in
  `errno` without changing copy exit status. CSV units are seconds for wall, CPU, and I/O-call time;
  decimal MB/s for throughput (`bytes / wall_s / 1,000,000`); microseconds for latency; KiB for peak
  RSS; milliseconds for occupancy time; and records for queue sizes.
- **Summary behavior**: `metrics_print_summary` emits the normal one-line stderr summary on every
  run. It uses the caller-supplied byte, block, and wall totals and the CPU/RSS values captured by
  merge. Disabled or unavailable latency and I/O-call fields print `n/a`; if `getrusage` failed,
  CPU/RSS fields print `n/a`. Linux `ru_maxrss` is in KiB.
- **Integration boundaries**: Phat owns pipeline lifecycle calls for start/origin, writer totals,
  registration, merge, CSV output, and final summary; those call sites are outside the metrics-module
  implementation. Vy owns the separate `io.c` timer hook, which requires her approval and a separate
  `docs/design.md` entry.
- **Review gate**: The review PR contains the draft `src/metrics.h` and this design entry. Phat and
  Vy re-review the PR; the gate task verifies that the PR is merged and this entry is present before
  implementation of `src/metrics.c` or `tests/test_metrics.c` begins. Finalize the header after the
  gate. This metrics-module gate is separate from changes to already agreed interfaces; changes to
  `io.h`, `common.h`, or Overview section 10 still require an all-three-approved PR and a design
  entry. Spec Kit artifacts remain local and do not create or publish GitHub work.
