# pipe-chunk

[![ci](https://github.com/owenng71/pipe-chunk/actions/workflows/ci.yml/badge.svg)](https://github.com/owenng71/pipe-chunk/actions/workflows/ci.yml)

A parallel file copier in C11 and POSIX threads, built to study **bounded buffers, backpressure and
robust I/O** on Linux.

A **reader** thread splits the input into fixed-size blocks, **worker** threads compute a SHA-256
checksum for each block, and a **writer** thread `pwrite`s every block to its offset in the output
and records `seq,offset,len,sha256` in a manifest. Bounded queues and a fixed buffer pool keep memory
constant when one stage is slower than the others. The copy must stay correct under partial reads
and writes, interrupted system calls, a slow output device, a crashing worker and Ctrl+C.

> **Status: in development.** The build, CLI, CI and the bounded queue are done; the I/O layer,
> buffer pool and both run modes are in progress. See [Roadmap](#roadmap).

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
| worker dies holding a block | supervisor → Q1 (retry, at most `MAX_ATTEMPTS`) |

**Correctness without reordering.** Workers finish out of order. The writer uses `pwrite` at each
block's offset (idempotent) and keeps a bitmap of written sequence numbers, so a retried block is
never written or listed twice.

**Shutdown.** At end of input: close Q1 → workers drain and exit → close Q2 → writer drains →
`fsync`. The first Ctrl+C stops reading and finishes every accepted block (exit code 130); a second
Ctrl+C stops immediately.

**Robust I/O.** Every `read`/`write` goes through one module (`io.c`) that retries short counts,
`EINTR` and `EAGAIN`. Fault injection (`--fault-*`, `--fail-worker`) forces those paths in tests.

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
./build/pipe-chunk --mode=pipeline -w 4 -q 16 -b 64K /tmp/in.bin /tmp/out.bin
./scripts/verify.sh /tmp/in.bin /tmp/out.bin                    # prints VERIFY OK or VERIFY FAIL
```

`verify.sh` passes only if the output is byte-identical to the input and the manifest lists every
block exactly once with no gaps or overlaps.

| Option | Meaning |
|---|---|
| `-b, --block-size=BYTES` | block size, `K`/`M`/`G` suffixes allowed (default 64 KiB) |
| `-q, --queue=N` | capacity of Q1 and Q2 (default 16) |
| `-w, --workers=N` | worker threads (default 4) |
| `--mode=blocking\|pipeline` | single-threaded baseline, or the concurrent pipeline (default) |
| `--io=rw\|mmap` | read input with `read()` into pool buffers, or `mmap` it |
| `--unbounded` | disable backpressure (memory-growth demonstration) |
| `--in-rate`, `--out-rate=MB/s` | throttle the reader or the writer |
| `--fault-short`, `--fault-eintr`, `--fault-eagain=P` | inject short I/O, `EINTR`, `EAGAIN` with probability P |
| `--fault-delay-us=US`, `--fail-worker=P`, `--seed=N` | extra I/O delay, worker crashes, reproducible fault runs |
| `--manifest=FILE`, `--metrics=PREFIX` | manifest path; write summary and queue-occupancy CSVs |

`INPUT` may be `-` to read from a pipe on stdin (nonblocking, with `poll()`). Run
`./build/pipe-chunk --help` for the full list.

## Testing

| Command | What it runs |
|---|---|
| `make test` | every unit test in `tests/` |
| `make SAN=thread test` | the same under ThreadSanitizer (data races) |
| `make SAN=address test` | the same under AddressSanitizer (memory errors and leaks) |

CI runs all three on every push. If ThreadSanitizer aborts with `unexpected memory mapping` on
Ubuntu 24.04, run `sudo sysctl vm.mmap_rnd_bits=28` once per boot.

## Repository layout

| Path | Contents |
|---|---|
| `src/main.c` | command-line parsing and mode selection |
| `src/pipeline.c` | thread wiring, supervisor, shutdown order (`--mode=pipeline`) |
| `src/baseline.c` | single-threaded reference implementation (`--mode=blocking`) |
| `src/queue.c` | bounded blocking queue: ring buffer, mutex, `not_full`/`not_empty` condition variables |
| `src/pool.c` | fixed pool of pre-allocated block buffers |
| `src/io.c` | `read_full` / `pwrite_full`, the only place that calls `read`/`write` |
| `src/common.h` | `record_t` (one block in transit) and `config_t` (all options) |
| `src/metrics.h` | metrics interface: latency percentiles, queue occupancy, CPU, memory, I/O time |
| `tests/` | one `test_<module>.c` per module; plain `main()` with `assert` |
| `scripts/` | `setup.sh`, `gen_input.sh`, `verify.sh` |

## Roadmap

- [x] Build system, CLI, CI with ThreadSanitizer and AddressSanitizer
- [x] Bounded blocking queue with close/broadcast semantics and stress tests
- [ ] Robust I/O layer (`read_full`, `pwrite_full`)
- [ ] Buffer pool
- [ ] Blocking baseline and end-to-end check in CI
- [ ] Concurrent pipeline with backpressure, `--unbounded` demonstration
- [ ] Metrics (throughput, p50/p99 latency, occupancy, CPU, peak RSS) and rate limiting
- [ ] Fault injection, worker supervisor, graceful Ctrl+C, nonblocking stdin, `mmap` input
- [ ] End-to-end fault matrix in CI
- [ ] Benchmarks: queue size × workers × block size, `rw` vs `mmap`, bounded vs unbounded memory

## Authors

Operating Systems course project. Led by Phat ([@owenng71](https://github.com/owenng71)); bounded
queue by Son.
