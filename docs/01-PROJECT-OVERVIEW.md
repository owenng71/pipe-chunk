# 01 — Project Overview: Asynchronous I/O Pipeline with Bounded Buffers and Backpressure

> **Who this is for:** every team member (Phat, Vy, Son) and any AI assistant (Claude) helping on the repo.
> Read this first. It explains **what** we are building and **why**. The *how/when/who* is in `02-PLAN-AND-ROLES.md`.

---

## 1. The assignment in one paragraph

Course project #11 (area: *Input/output devices*). We build a high-throughput local pipeline that
**reads** file blocks from an input, **processes** them with worker threads, and **writes** results to an
output. It must stay **correct** (no lost or duplicated data) and keep **memory bounded** when the input is
faster than the consumer, the output is slow, reads/writes are partial, or a stage fails. Then we **measure**
it and **explain** the throughput/latency trade-offs.

Core OS topics we must demonstrate and every member must be able to explain:
blocking vs nonblocking I/O · buffering · producer–consumer synchronization · backpressure · partial I/O ·
I/O performance · why a pipeline without flow control eats memory.

---

## 2. What our program does

Program name: **`pipe-chunk`** (C11, Linux/Ubuntu, pthreads).

It is a **parallel file copier with per-block checksums**:

1. Split the input file into fixed-size **blocks** (e.g. 64 KB).
2. Compute a **SHA-256 fingerprint** of each block using several worker threads in parallel.
3. Write each block to the output file at its correct position, and write a **manifest** line
   `seq,offset,len,sha256` for every block.

**Why this workload?** Correctness is trivially provable:

- `sha256sum input == sha256sum output` → the copy is byte-perfect.
- The manifest contains every `seq` from `0..N-1` **exactly once** → nothing lost, nothing duplicated.

This one check is run after *every* test and *every* injected failure.

---

## 3. The mental model: a pizza kitchen

```
 Fridge guy ──▶ [shelf, max Q spots] ──▶ Cooks (N) ──▶ [shelf, max Q spots] ──▶ Delivery driver
 (Reader)          (Queue 1)            (Workers)        (Queue 2)              (Writer)
     ▲                                                                            │
     └──────────────── Tray pool (fixed number of trays) ◀── trays returned ──────┘
```

| Kitchen | Program |
|---|---|
| Fridge (raw dough) | Input file (or a pipe on stdin) |
| Fridge guy | **Reader thread** — reads one block at a time |
| Cooks | **Worker threads** — compute SHA-256 per block |
| Delivery driver | **Writer thread** — `pwrite`s block to output, appends manifest line |
| Shelf with limited spots | **Bounded queue** |
| Tray | **Buffer** (a `record_t` holding one block) |
| Ticket number | **Sequence number** `seq` |
| Manager | **Supervisor** — detects dead workers, re-queues their block, hires a replacement |

**Backpressure in one sentence:** if the driver is slow, shelf 2 fills → cooks wait → shelf 1 fills →
fridge guy waits → everyone runs at the speed of the slowest stage, and memory never exceeds
`trays × tray_size`.

**Without backpressure** (`--unbounded`): the fridge guy never stops, pizzas pile up everywhere, memory
grows until the machine runs out. We demonstrate this on purpose with a graph.

---

## 4. Key concepts (short glossary)

| Term | Meaning in our project |
|---|---|
| **Blocking I/O** | `read()` puts the thread to sleep until data is ready. Simple; thread can't do anything else. |
| **Nonblocking I/O** | fd opened with `O_NONBLOCK`; `read()` returns `-1/EAGAIN` ("not ready") instantly. |
| **`poll()`** | "Wake me when this fd is ready" — efficient waiting instead of busy-looping (event-driven). |
| **Buffer** | Memory area holding one block in transit. |
| **Bounded queue** | Ring buffer with max capacity; push blocks when full, pop blocks when empty. |
| **Mutex** | "Bathroom key": only the holder may touch the shared queue. |
| **Condition variable** | "Waiting bench with a bell": `not_full` (producers sleep here), `not_empty` (consumers sleep here). |
| **Producer / consumer** | Who puts items into / takes items out of a queue. |
| **Backpressure** | A full downstream queue makes upstream stages block → bounded memory. |
| **Short read/write** | Kernel transfers fewer bytes than asked. Must loop for the rest. |
| **EOF** | `read()` returns `0` → no more input. |
| **`EINTR`** | Syscall interrupted by a signal. Not an error — retry. |
| **`EAGAIN`** | Nonblocking fd not ready. Not an error — `poll()` then retry. |
| **Idempotent write** | `pwrite(fd, buf, len, offset)` writes at an exact position; doing it twice gives the same file. |
| **`mmap`** | Map the file into memory and read it directly, instead of copying it with `read()`. |
| **Zero-copy** | Kernel moves data file→file without passing through our memory (`copy_file_range`, `splice`). |
| **Vectored I/O** | One syscall writes several buffers (`pwritev`). |
| **Fault injection** | Deliberately faking short I/O, `EINTR`, `EAGAIN`, delays, worker crashes to prove robustness. |

---

## 5. Architecture & design decisions

### 5.1 The record (one tray)

```c
typedef struct record {
    uint64_t       seq;          // block number 0..N-1
    off_t          offset;       // byte position in input/output
    size_t         len;          // bytes actually in buf (last block may be shorter)
    size_t         cap;          // buffer capacity (= block size)
    unsigned char *buf;          // the data
    unsigned char  sha[32];      // SHA-256 of buf[0..len)
    uint64_t       t_read_ns;    // timestamp when read (for latency)
    int            attempts;     // how many times a worker tried this record
} record_t;
```

### 5.2 Buffer ownership rules (the "hot potato" contract)

A record is owned by **exactly one** stage at any moment. Only the owner may read or modify it.

| Step | Ownership transfer |
|---|---|
| Reader calls `pool_get()` | Pool → Reader |
| Reader `bq_push(Q1, r)` | Reader → Q1 (reader must not touch `r` anymore) |
| Worker `bq_pop(Q1)` | Q1 → Worker (supervisor records it as the worker's *in-flight* record) |
| Worker `bq_push(Q2, r)` | Worker → Q2 |
| Writer `bq_pop(Q2)` | Q2 → Writer |
| Writer `pool_put(r)` after `pwrite` + manifest | Writer → Pool |
| Worker dies holding `r` | Supervisor takes `r` and re-pushes it to Q1 |

Nothing is copied between stages; only the pointer moves.

### 5.3 Synchronization table

| Shared thing | Protected by | Who waits, on what |
|---|---|---|
| Queue Q1, Q2 | 1 mutex per queue | Producers on `not_full`, consumers on `not_empty` |
| Buffer pool | 1 mutex + `not_empty` condvar | Reader waits when no free tray |
| Worker in-flight slots | Supervisor mutex | — |
| Writer "already written" bitmap | Owned by writer thread only (no lock) | — |
| Cancel flag | `atomic_bool` | Checked by reader loop |
| Metrics counters | `atomic` ints / per-thread counters merged at end | — |

`bq_close(q)` sets `closed = true` and broadcasts both condvars so every sleeping thread wakes up and exits.

### 5.4 Ordering

Workers finish out of order. We **do not reorder**: the writer uses `pwrite` at `record->offset`, so
order doesn't matter. The writer keeps a **bitmap of written `seq`** and drops any duplicate — a second
safety net so the manifest is always exactly-once.

### 5.5 Worker failure (stage failure)

- Each worker has a slot `{thread, in_flight record, state}` managed by the **supervisor**.
- Injected failure (`--fail-worker=P`): with probability P the worker exits mid-record **without** pushing it.
- On exit, a worker reports its slot id to the supervisor. The supervisor `pthread_join`s it; if it died
  abnormally it (1) **spawns a replacement first**, then (2) re-pushes the in-flight record to Q1
  (`attempts++`). Replacement-first prevents a deadlock where Q1 is full and nobody is consuming.
- If a record fails more than `MAX_ATTEMPTS` (3), the pipeline aborts with an error instead of looping forever.
- Because `pwrite` is idempotent and the writer bitmap drops duplicates → **no loss, no duplication**.

### 5.6 Cancellation (Ctrl+C)

- All threads block signals; one dedicated **signal thread** uses `sigwait()`.
- **First SIGINT/SIGTERM = graceful drain:** stop the reader (no new records accepted), close Q1,
  let workers and writer finish every **accepted** record (one that entered Q1), `fsync` output +
  manifest, exit code `130`.
- **Second SIGINT = hard stop:** exit immediately; the manifest still lists exactly the completed blocks.
- "Accepted record" = a record that was pushed into Q1. Accepted records are never lost.

### 5.7 End-of-file shutdown order

Reader hits EOF → `bq_close(Q1)` → workers drain Q1 and exit → supervisor joins all workers →
`bq_close(Q2)` → writer drains Q2 and exits → `fsync`, print metrics.

### 5.8 Memory bound

`pool size = Q1_cap + Q2_cap + workers + 2` trays (default). Peak data memory ≈ `pool_size × block_size`.
With `--unbounded`, queues have no capacity and the pool `malloc`s a new tray on demand (the demo).

---

## 6. Modes and I/O strategies

| Option | Meaning |
|---|---|
| `--mode=blocking` | **Baseline:** one thread, loop read → sha → pwrite. Required comparison point. |
| `--mode=pipeline` | **Concurrent version:** reader + N workers + writer with bounded queues (default). |
| `--io=rw` | **Strategy A:** blocking `read()` into pool buffers. |
| `--io=mmap` | **Strategy B:** `mmap` the input; workers hash straight from the mapping. |
| input `-` (stdin pipe) | **Nonblocking path:** `O_NONBLOCK` + `poll()`, handles `EAGAIN`. |
| `--zerocopy` / `--vectored` | **Stretch:** `copy_file_range` or `pwritev` batching on the write path. |

## 7. Command-line interface (contract)

```
pipe-chunk [options] INPUT OUTPUT
  -b, --block-size=BYTES     block size (default 65536)
  -q, --queue=N              capacity of Q1 and Q2 (default 16)
  -w, --workers=N            worker threads (default 4)
      --mode=blocking|pipeline
      --io=rw|mmap
      --unbounded            disable backpressure (memory-growth demo)
      --in-rate=MB/s         throttle reader      --out-rate=MB/s  throttle writer
      --fault-short=P        prob. of short read/write
      --fault-eintr=P        prob. of fake EINTR
      --fault-eagain=P       prob. of fake EAGAIN
      --fault-delay-us=US    random extra delay per I/O
      --fail-worker=P        prob. a worker dies mid-record
      --seed=N               RNG seed (makes fault runs reproducible)
      --manifest=FILE        default OUTPUT.manifest
      --metrics=PREFIX       writes PREFIX.summary.csv and PREFIX.occupancy.csv
```

## 8. What we measure

| Metric | How |
|---|---|
| Throughput (MB/s) | total bytes ÷ wall time |
| End-to-end latency p50/p99 | `now - t_read_ns` when writer finishes a record |
| Queue occupancy | monitor thread samples `bq_size(Q1/Q2)` every 10 ms |
| CPU usage | `getrusage(RUSAGE_SELF)` user + sys time |
| Memory | peak RSS (`ru_maxrss`, `/proc/self/status` VmHWM) |
| I/O wait | time spent inside read/write/poll calls (our timers) + `vmstat`/`pidstat` iowait |

**Experiments:** queue ∈ {1, 4, 16, 64} × workers ∈ {1, 2, 4, 8} × block ∈ {4K, 64K, 1M}, plus slow
input/output via rate limits, `rw` vs `mmap`, bounded vs `--unbounded`. Each config is run 5× and we
report the median. Test files are larger than the page cache when measuring disk effects.

**The trade-off we must explain:** bigger queues → smoother, higher throughput but higher latency and
more memory; more workers → faster until the disk/writer becomes the bottleneck, then no gain.

---

## 9. Acceptance gate (what "done" means)

1. **Reproducible:** on a clean Ubuntu machine, `./scripts/setup.sh && make && make test` works with no
   undocumented manual steps (Docker wrapper will be added by Phat after the project is complete).
2. **Correct under stress:** `verify.sh` passes with partial I/O, a slow consumer, an injected worker
   failure, and Ctrl+C.
3. **Evaluated:** results vary at least buffer (queue) size and worker count, with graphs and an
   explanation of the throughput/latency trade-off.
4. **Understood:** every member can explain blocking vs nonblocking, buffering, backpressure,
   synchronization, and memory blow-up without flow control.

---

## 10. Repository layout

```
io-pipeline/
├── src/
│   ├── main.c           CLI parsing, mode selection
│   ├── pipeline.c/.h    thread wiring, supervisor, shutdown order        (Phat)
│   ├── queue.c/.h       bounded blocking queue                           (Son)
│   ├── pool.c/.h        buffer pool                                      (Son)
│   ├── cancel.c/.h      signal thread, drain logic                       (Son)
│   ├── metrics.c/.h     timers, monitor thread, CSV output               (Son)
│   ├── io.c/.h          read_full, pwrite_full, poll path, mmap          (Vy)
│   ├── faults.c/.h      fault-injection wrappers                         (Vy)
│   ├── ratelimit.c/.h   token bucket for --in-rate/--out-rate            (Vy)
│   └── baseline.c       --mode=blocking                                  (Vy)
├── tests/
│   ├── test_queue.c, test_pool.c, test_io.c   unit tests (module owners)
│   └── e2e.sh                                 fault/integration matrix (Phat)
├── scripts/  setup.sh, gen_input.sh, verify.sh                           (Phat)
├── bench/    sweep.sh, plot.py, results/                                 (Son)
├── docs/     01-PROJECT-OVERVIEW.md, 02-PLAN-AND-ROLES.md, design.md, report/
├── .github/workflows/ci.yml                                              (Phat)
├── Makefile
└── README.md
```

### Module interfaces (agreed in Week 1 so everyone can work in parallel)

```c
/* queue.h — Son */
bq_t  *bq_create(size_t capacity);          // capacity 0 = unbounded
int    bq_push(bq_t *q, record_t *r);       // blocks while full; -1 if closed
int    bq_pop(bq_t *q, record_t **out);     // blocks while empty; 0 = closed and empty
void   bq_close(bq_t *q);                   // wake everyone
size_t bq_size(bq_t *q);
void   bq_destroy(bq_t *q);

/* pool.h — Son */
pool_t   *pool_create(size_t nbufs, size_t bufsize);   // nbufs 0 = unbounded (malloc on demand)
record_t *pool_get(pool_t *p);                          // blocks while empty; NULL if closed
void      pool_put(pool_t *p, record_t *r);
void      pool_close(pool_t *p);

/* io.h — Vy */
ssize_t read_full(int fd, void *buf, size_t n);              // loops on short/EINTR/EAGAIN; <n only at EOF; -1 on error
int     pwrite_full(int fd, const void *buf, size_t n, off_t off); // 0 ok, -1 error

/* pipeline.h — Phat */
int pipeline_run(const config_t *cfg);      // returns exit code
```

---

## 11. Notes for Claude (and anyone writing code)

- Target: **Ubuntu 24.04, gcc, C11, pthreads**. Only external library: OpenSSL `libcrypto` (SHA-256), approved by the lecturer.
- Build flags: `-std=c11 -Wall -Wextra -O2 -g -pthread`; CI also builds with `-fsanitize=thread` and `-fsanitize=address`.
- Error convention: functions return `0`/`-1` and keep `errno`; never `exit()` from inside a module.
- Every syscall that can be short or interrupted goes through `io.c` helpers — never call raw `read`/`write` elsewhere.
- Respect the ownership table in §5.2. A data race is a bug even if tests pass.
- Keep it simple: no extra abstractions, no third-party libraries. Mark known shortcuts with a `// limit:` comment.
- Docker is **out of scope** for now; Phat adds it after the project is complete.
- Plan, owners and dates: see `02-PLAN-AND-ROLES.md`.
