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
- **Approval**: All three members approved the section 10 clarification in the PR; approval was
  confirmed by the user on 2026-10-24.
- **Status**: The required shared-contract review is complete and recorded.

## W1-09 — `pool_destroy` interface proposal

- **Decision**: Add `void pool_destroy(pool_t *p);` to Overview section 10. It frees every tray
  and each original buffer from the pool master list, including checked-out trays; it never frees a
  repointed `buf`. Destroy queues before the pool.
- **Affected interface**: `pool_destroy(pool_t *p)` in the pool interface.
- **Rationale**: Reclaim all pool-owned memory at shutdown while preserving ownership of any
  external buffer address currently stored in a tray's `buf` field. Destroying queues first removes
  queued record references before the pool frees tray storage.
- **Approval**: Pending a pull request approved by all three team members.
- **Status**: Proposed; do not treat as an approved shared contract until that review is complete.
