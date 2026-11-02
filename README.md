# pipe-chunk

[![ci](https://github.com/owenng71/pipe-chunk/actions/workflows/ci.yml/badge.svg)](https://github.com/owenng71/pipe-chunk/actions/workflows/ci.yml)

A parallel file copier in C11 and POSIX threads, built to study **bounded buffers, backpressure and
robust I/O** on Linux.

A **reader** thread splits the input into fixed-size blocks, **worker** threads compute a SHA-256
checksum for each block, and a **writer** thread `pwrite`s every block to its offset in the output
and records `seq,offset,len,sha256` in a manifest. Bounded queues and a fixed buffer pool keep memory
constant when one stage is slower than the others. The copy must stay correct under partial reads
and writes, interrupted system calls, a slow output device, a crashing worker and Ctrl+C.

> **Status: in development.** The concurrent pipeline with backpressure, the blocking baseline,
> rate limiting, metrics, I/O fault injection, worker-failure recovery, graceful Ctrl+C,
> nonblocking stdin input and `mmap` input are checked in CI, including an end-to-end fault matrix. Benchmarks are next. See [Roadmap](#roadmap).

---

## Architecture

```
            ┌──────────── buffer pool (fixed number of trays) ◀────────────┐
            ▼                                                              │
 input ─▶ Reader ─▶ [ Q1: bounded ] ─▶ Workers ×N ─▶ [ Q2: bounded ] ─▶ Writer ─▶ output + manifest
                                       (SHA-256)                       (pwrite at offset)
                                           ▲
                                       Supervisor (respawns a dead worker, re-queues its block)
```

**Backpressure.** If the writer is slow, Q2 fills and workers block; then Q1 fills and the reader
blocks. Every stage runs at the speed of the slowest one, and data memory never exceeds
`trays × block_size`. `--unbounded` turns this off to show memory growing without flow control.

**Buffer ownership.** A block (`record_t`) is owned by exactly one stage at a time and only its
pointer moves between stages, never the data:

| Step | Ownership |
|---|---|
| `pool_get()` | pool → reader |
| `bq_push(Q1)` / `bq_pop(Q1)` | reader → Q1 → worker |
| `bq_push(Q2)` / `bq_pop(Q2)` | worker → Q2 → writer |
| `pool_put()` after `pwrite` + manifest line | writer → pool |
| `--io=mmap`: reader points `buf` into the mapped input instead of reading into the tray | data is never copied; `pool_get` gives the tray its own buffer back |
| worker dies holding a block | supervisor → replacement worker (retry, at most `MAX_ATTEMPTS` tries) |

**Correctness without reordering.** Workers finish out of order. The writer uses `pwrite` at each
block's offset (idempotent) and keeps a bitmap of written sequence numbers, so a retried block is
never written or listed twice.

**Shutdown.** At end of input: close Q1 → workers drain and exit → close Q2 → writer drains →
`fsync`. The first Ctrl+C stops reading and finishes every accepted block (exit code 130); a second
Ctrl+C stops immediately.

**Robust I/O.** Every `read`/`write` goes through one module (`io.c`) that retries short counts,
`EINTR` and `EAGAIN`. Fault injection (`--fault-*`, `--fail-worker`) forces those paths in tests.

**`mmap` input.** With `--io=mmap` the input file is mapped once, read-only, and each block points
into the mapping, so workers hash and the writer `pwrite`s with no copy. Trays still limit how many
blocks are in flight. Peak RSS includes the mapped file pages that were touched (clean page cache the
kernel can drop), so it reaches the file size even though backpressure still holds. If another
program truncates the input mid-copy, the process gets `SIGBUS`; `read()` would only see an early EOF.

---

## Build

Requirements: Linux (Ubuntu 24.04 tested), gcc, make, OpenSSL `libcrypto`.

```sh
git clone https://github.com/owenng71/pipe-chunk.git
cd pipe-chunk
./scripts/setup.sh        # installs build-essential, libssl-dev, python3 (uses sudo)
make                      # builds build/pipe-chunk
```

On Windows, use WSL 2 with Ubuntu 24.04 and clone into the Linux home directory, not `/mnt/c`.

## Usage

```sh
./scripts/gen_input.sh 100M /tmp/in.bin                         # random 100 MB test file
./build/pipe-chunk -w 4 -q 16 -b 64K /tmp/in.bin /tmp/out.bin    # writes out.bin + out.bin.manifest
./scripts/verify.sh /tmp/in.bin /tmp/out.bin                    # prints VERIFY OK or VERIFY FAIL
```

Every run ends with a one-line summary on stderr:

```
pipe-chunk: 1073741824 bytes in 16384 blocks, 0.507 s, 2118.1 MB/s; latency p50 655 us, p99 1038 us;
cpu user 0.525 s, sys 0.567 s; peak RSS 7168 KiB; I/O calls 0.408 s
```

`--metrics=PREFIX` also writes `PREFIX.summary.csv` (the same numbers) and `PREFIX.occupancy.csv`
(queue sizes sampled every 10 ms).

`verify.sh` passes only if the output is byte-identical to the input and the manifest lists every
block exactly once with no gaps or overlaps.

| Option | Meaning |
|---|---|
| `-b, --block-size=BYTES` | block size, `K`/`M`/`G` suffixes allowed (default 64 KiB) |
| `-q, --queue=N` | capacity of Q1 and Q2 (default 16) |
| `-w, --workers=N` | worker threads (default 4) |
| `--mode=blocking\|pipeline` | single-threaded baseline, or the concurrent pipeline (default) |
| `--io=rw\|mmap` | read input with `read()` into pool buffers, or `mmap` it and hash and write straight from the mapping (regular files only) |
| `--unbounded` | disable backpressure (memory-growth demonstration) |
| `--in-rate`, `--out-rate=MB/s` | throttle the reader or the writer |
| `--fault-short`, `--fault-eintr`, `--fault-eagain=P` | inject short I/O, `EINTR`, `EAGAIN` with probability P |
| `--fault-delay-us=US`, `--fail-worker=P`, `--seed=N` | extra I/O delay, worker crashes, reproducible fault runs |
| `--manifest=FILE`, `--metrics=PREFIX` | manifest path; write summary and queue-occupancy CSVs |

`INPUT` may be `-` to read from stdin. A pipe or socket on stdin is switched to `O_NONBLOCK` for the
run: when the producer is slow, `read` returns `EAGAIN` and the reader waits in `poll()`. The
original flags are restored at the end. A terminal or redirected file is read as it is. Run
`./build/pipe-chunk --help` for the full list.

## Testing

| Command | What it runs |
|---|---|
| `make test` | every unit test in `tests/` |
| `make SAN=thread test` | the same under ThreadSanitizer (data races) |
| `make SAN=address test` | the same under AddressSanitizer (memory errors and leaks) |
| `./tests/e2e.sh [BINARY]` | end-to-end copies in both modes (100 MiB, empty, uneven sizes, many worker/queue/block settings, unbounded, slow input, stdin pipe, `mmap`), a fault matrix (short I/O, `EINTR`, `EAGAIN`, delays, dying workers, Ctrl+C drain and hard stop, each in several modes), each checked by `verify.sh`, plus a backpressure check: with a slow writer, bounded mode must stay under 32 MB while `--unbounded` grows |

The I/O unit test replaces `read` and `pwrite` with scripted versions to force short counts,
`EINTR`, `EAGAIN` and zero-byte writes deterministically, and also uses real signals and
nonblocking pipes. CI runs the unit tests and `e2e.sh` for the plain, ThreadSanitizer and
AddressSanitizer builds on every push. If ThreadSanitizer aborts with `unexpected memory mapping` on
Ubuntu 24.04, run `sudo sysctl vm.mmap_rnd_bits=28` once per boot.

## Benchmarks

```sh
make && ./bench/sweep.sh                                   # full sweep, about 10 minutes
SIZE=64M RUNS=1 BLOCKS=64K ./bench/sweep.sh                # quick look
```

`bench/sweep.sh` copies a 1 GiB file with every combination of queue size {1, 4, 16, 64}, workers
{1, 2, 4, 8} and block size {4K, 64K, 1M}, plus the blocking baseline for each block size, 5 runs
each. Every run is checked with `verify.sh`. Runs go round-robin over the configurations so drift on
the machine does not bias one of them. Input and output live in `/dev/shm` (RAM), so the sweep
measures CPU and synchronization costs rather than the disk: on an SSD, flushing each 1 GiB copy
made the same configuration vary 2–3× between runs. Set `WORK=DIR` to run on a disk instead.
Results go to `bench/results/`:

| File | Contents |
|---|---|
| `sweep.csv` | one row per configuration: median throughput, p50/p99 latency, CPU, peak RSS, I/O time, and min/max throughput |
| `sweep_raw.csv` | one row per run |
| `occupancy/*.csv` | queue sizes every 10 ms for the first run of each pipeline configuration |
| `sweep_env.txt` | CPU, governor, kernel, compiler, commit and settings |

`SIZE`, `RUNS`, `QUEUES`, `WORKERS`, `BLOCKS` and `OUT` override the defaults, e.g.
`QUEUES="4 16" WORKERS="2 4"`.

## Repository layout

| Path | Contents |
|---|---|
| `src/main.c` | command-line parsing and mode selection |
| `src/pipeline.c` | thread wiring, supervisor, shutdown order (`--mode=pipeline`) |
| `src/baseline.c` | single-threaded reference implementation (`--mode=blocking`) |
| `src/queue.c` | bounded blocking queue: ring buffer, mutex, `not_full`/`not_empty` condition variables |
| `src/pool.c` | fixed pool of pre-allocated block buffers |
| `src/io.c` | `read_full` / `pwrite_full`, the only place that calls `read`/`write`; times every call |
| `src/files.c` | opening input (nonblocking stdin pipe or `mmap`), output and manifest; block reads; manifest lines; sync and close |
| `src/seqset.c` | growable bitmap of written block numbers: the writer drops a repeated block |
| `src/faults.c` | fault injection for `io.c` (`--fault-*`, `--seed`) |
| `src/cancel.c` | `sigwait` signal thread: first Ctrl+C drains, second stops at once |
| `src/ratelimit.c` | pacing for `--in-rate` / `--out-rate` (slow-device simulation) |
| `src/metrics.c` | latency percentiles, queue-occupancy monitor thread, CPU, peak RSS, I/O time, CSV |
| `src/common.h` | `record_t` (one block in transit) and `config_t` (all options) |
| `tests/` | one `test_<module>.c` per module; plain `main()` with `assert` |
| `tests/e2e.sh` | end-to-end runs of the binary, each verified |
| `scripts/` | `setup.sh`, `gen_input.sh`, `verify.sh` |
| `bench/sweep.sh` | parameter sweep: queue × workers × block size, median of 5 runs, CSV results |

## Roadmap

- [x] Build system, CLI, CI with ThreadSanitizer and AddressSanitizer
- [x] Bounded blocking queue with close/broadcast semantics and stress tests
- [x] Robust I/O layer (`read_full`, `pwrite_full`)
- [x] Buffer pool
- [x] Blocking baseline and end-to-end check in CI
- [x] Concurrent pipeline with backpressure, `--unbounded` demonstration
- [x] Metrics (throughput, p50/p99 latency, occupancy, CPU, peak RSS) and rate limiting
- [x] Graceful Ctrl+C: first drains accepted blocks (exit 130), second stops immediately
- [x] I/O fault injection (short counts, `EINTR`, `EAGAIN`, delays) with `--seed`
- [x] Worker supervisor: dead workers replaced, their block retried (`--fail-worker`)
- [x] Nonblocking stdin pipe input (`O_NONBLOCK` + `poll()`)
- [x] `mmap` input: zero-copy, blocks point into the mapping
- [x] End-to-end fault matrix in CI
- [ ] Benchmarks: queue size × workers × block size, `rw` vs `mmap`, bounded vs unbounded memory
