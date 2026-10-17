# pipe-chunk: asynchronous I/O pipeline with bounded buffers and backpressure

OS course project #11 (area: input/output devices). Team: **Phat · Vy · Son**.

`pipe-chunk` is a parallel file copier. A **reader** thread splits the input file into blocks,
**worker** threads compute a SHA-256 checksum for each block, and a **writer** thread `pwrite`s every
block to the output and appends one line `seq,offset,len,sha256` to a manifest. Bounded queues and a
fixed buffer pool keep memory bounded when one stage is slower than the others (**backpressure**).
The program must stay correct under partial I/O, slow output, a crashing worker and Ctrl+C.

| Read this | For |
|---|---|
| [docs/01-PROJECT-OVERVIEW.md](docs/01-PROJECT-OVERVIEW.md) | **What** we build and **why**: concepts, architecture, buffer ownership, CLI |
| [docs/02-PLAN-AND-ROLES.md](docs/02-PLAN-AND-ROLES.md) | **Who** does **what**, **when**: roles, 3 sprints, every task, GitHub workflow |
| GitHub **Issues** / **Projects** tab | Live status of every task (one issue per task ID, e.g. `W1-08 Bounded queue`) |

---

## 1. Set up your machine (no Docker needed)

Target platform: **Ubuntu 24.04** with gcc, make and OpenSSL `libcrypto`. That is everything.
macOS and native Windows are **not** supported (we use Linux-only syscalls and `/proc`).

### 1a. You run Ubuntu (native, VM or dual boot)

```sh
sudo apt-get update && sudo apt-get install -y git
git clone https://github.com/owenng71/pipe-chunk.git   # private repo: ask Phat to add you as collaborator
cd pipe-chunk
./scripts/setup.sh        # installs build-essential, libssl-dev, python3 (asks for your sudo password)
```

### 1b. You run Windows → use WSL 2

1. Open **PowerShell as Administrator** and run:
   ```powershell
   wsl --install -d Ubuntu-24.04
   ```
   Restart if asked, then open **Ubuntu 24.04** from the Start menu and choose a Linux username and password.
2. Inside the Ubuntu terminal, follow **1a** exactly.

> Clone into your Linux home (`~/pipe-chunk`), **not** into `/mnt/c/...` or `/mnt/d/...`.
> Windows drives under WSL are slow and lose the executable bit on scripts.
> In VS Code: install the **WSL** extension, then run `code .` from the Ubuntu terminal.

### 1c. Check that it works

```sh
make                      # builds build/pipe-chunk, no warnings expected
make test                 # runs every unit test in tests/
./build/pipe-chunk --help # prints all options
```

If `make` says `openssl/sha.h: No such file`, you skipped `./scripts/setup.sh`.

---

## 2. Build, test, run

| Command | What it does |
|---|---|
| `make` | Build `build/pipe-chunk` |
| `make test` | Build and run every `tests/test_*.c` |
| `make SAN=thread test` | Same, under **ThreadSanitizer** (finds data races), output in `build-thread/` |
| `make SAN=address test` | Same, under **AddressSanitizer** (finds memory bugs), output in `build-address/` |
| `make clean` | Delete all build folders |

CI runs the plain, `thread` and `address` builds on every pull request. If it fails in CI, run the same `make SAN=...` locally.

> ThreadSanitizer crash `FATAL: ThreadSanitizer: unexpected memory mapping` on Ubuntu 24.04?
> Run `sudo sysctl vm.mmap_rnd_bits=28` once per boot.

Run the program end to end:

```sh
./scripts/gen_input.sh 100M /tmp/in.bin                       # random 100 MB test file
./build/pipe-chunk --mode=blocking /tmp/in.bin /tmp/out.bin   # writes out.bin + out.bin.manifest
./scripts/verify.sh /tmp/in.bin /tmp/out.bin                  # prints VERIFY OK or VERIFY FAIL
```

`verify.sh` passes only if the output is byte-identical to the input and the manifest lists every
block exactly once. Every test and every fault-injection run is checked with it.

---

## 3. What each file is for

Each module is one `.c` file with a `.h` file next to it. The `.h` holds the **agreed interface**
(Overview §10). Change it only through a PR that all three members approve.
Status ✅ = in the repo · 🧱 = stub (compiles, returns an error, owner fills it in) · ⏳ = owner creates it in that task.

### `src/`: the program

| File | Owner · task | Status | What it does |
|---|---|---|---|
| `common.h` | All · W1-03 | ✅ | Shared types used by every module. `record_t` is one block in transit (seq, offset, len, buffer, SHA-256, timestamps, retry count). `config_t` holds every command-line option. `MAX_ATTEMPTS`. |
| `main.c` | Phat · W1-02 | ✅ | Entry point. Parses the command line into a `config_t` with `getopt_long`, rejects bad values (exit code 2), then calls `baseline_run()` or `pipeline_run()`. No I/O or threads here. |
| `pipeline.h` | Phat | ✅ | Declares the two run modes: `pipeline_run()` and `baseline_run()`. Both return the process exit code. |
| `pipeline.c` | Phat · W2-01, W2-05 | 🧱 | The concurrent version. Creates the pool, Q1 and Q2. Starts the reader, N workers and the writer. The writer `pwrite`s each block at its offset and drops duplicates with a seq bitmap. Contains the **supervisor**, which joins a dead worker, spawns a replacement and re-queues its block. Owns the shutdown order (EOF → close Q1 → join workers → close Q2 → join writer → fsync). |
| `baseline.c` | Vy · W1-10 | 🧱 | `--mode=blocking`: one thread, loop of read → SHA-256 → pwrite → manifest line. The reference for correctness and speed. |
| `queue.h/.c` | Son · W1-08, W2-09 | 🧱 | **Bounded blocking queue** (ring buffer + mutex + `not_full`/`not_empty` condition variables). `bq_push` blocks while full, which is what creates backpressure. `bq_pop` blocks while empty. `bq_close` wakes every waiting thread. Capacity 0 = unbounded (demo). |
| `pool.h/.c` | Son · W1-09, W2-09 | 🧱 | **Buffer pool**: a fixed number of pre-allocated `record_t` trays. `pool_get` blocks when every tray is in use, which caps memory at `trays × block_size`. `nbufs` 0 = malloc on demand (unbounded demo). |
| `io.h/.c` | Vy · W1-07, W2-07, W2-08 | 🧱 | The only place that calls `read`/`write`. `read_full` loops until it has `n` bytes or hits EOF, retrying on short reads, `EINTR` and `EAGAIN`. `pwrite_full` does the same for writes at an offset. Later: nonblocking stdin with `poll()` and `--io=mmap`. |
| `faults.h/.c` | Vy · W2-02 | ⏳ | Fault injection. Wrappers around the real syscalls that, with the probabilities from `--fault-*`, fake short reads/writes, `EINTR`, `EAGAIN` and delays. `--seed` makes a run reproducible. |
| `ratelimit.h/.c` | Vy · W2-03 | ⏳ | Token bucket used by `--in-rate` / `--out-rate` to make the reader or writer artificially slow (slow-disk simulation). |
| `cancel.h/.c` | Son · W2-04 | ⏳ | Ctrl+C handling. A dedicated signal thread uses `sigwait`. First SIGINT = graceful drain (stop reading, finish every accepted block, exit 130). Second SIGINT = hard stop. |
| `metrics.h/.c` | Son · W2-06 | ⏳ | Measurements: per-block latency (p50/p99), a monitor thread sampling queue occupancy every 10 ms, CPU and peak memory via `getrusage`, I/O-wait timers, CSV output for `--metrics=PREFIX`. |

### Everything else

| Path | Owner | What it is |
|---|---|---|
| `tests/test_<module>.c` | Module owner | Unit test for one module. Plain `main()` with `assert`s, exit 0 = pass. `make test` picks it up automatically by name. |
| `tests/e2e.sh` | Phat · W2-10 | End-to-end fault matrix: runs the binary under each fault option and checks each run with `verify.sh`. |
| `scripts/setup.sh` | Phat | Installs every build dependency on Ubuntu. |
| `scripts/gen_input.sh` | Phat | `gen_input.sh SIZE FILE`: random test file (e.g. `100M`, `1G`, `1000001` for an uneven last block). |
| `scripts/verify.sh` | Phat | `verify.sh IN OUT [MANIFEST]`: the correctness proof (byte-identical + every block exactly once). |
| `scripts/github_bootstrap.sh` | Phat | One-time: creates the GitHub labels, sprint milestones, one issue per task and the project board. |
| `bench/` | Son · W3-01, W3-02 | `sweep.sh` (parameter sweeps), `plot.py` (graphs), `results/` (CSV output, not committed). |
| `docs/` | All | Overview, plan, `design.md` (interface/behavior changes), final report. |
| `.github/workflows/ci.yml` | Phat | CI: build + unit tests, plain / ThreadSanitizer / AddressSanitizer, on every PR. |
| `Makefile` | Phat | Build rules (section 2). Any new `src/*.c` or `tests/test_*.c` is picked up automatically, so you never need to edit it to add a module. |

---

## 4. How we work (short version; full rules in [Plan §2](docs/02-PLAN-AND-ROLES.md))

1. Pick your issue on the board, assign yourself, move it to **In Progress**.
2. `git switch -c feat/w1-08-queue` (branch = `feat/<task-id>-<short-name>`, or `fix/...`).
3. Code + unit test. Before pushing: `make && make test && make SAN=thread test`.
4. Push and open a PR with `Closes #<issue number>`. The PR template has the Definition-of-Done checklist.
5. 1 approval + green CI → **squash-merge** and delete the branch.
   Reviewers: Vy's PRs → Son · Son's PRs → Vy · Phat's PRs → either.

Rules for code (from Overview §11): C11, `-Wall -Wextra` clean; functions return `0`/`-1` and keep
`errno`; never `exit()` inside a module; no raw `read`/`write` outside `io.c`; mark known shortcuts with `// limit:`.
