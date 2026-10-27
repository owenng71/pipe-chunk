# 02 — Plan, Roles & Sprints

> **What this is:** *how* we build the project described in `01-PROJECT-OVERVIEW.md`: who owns what,
> how we work on GitHub, and the 3 weekly sprints. Each task says **which problem it solves**.
>
> **Timeline:** Week 1 = Mon 12 Oct – Sun 18 Oct 2026 · Week 2 = 19–25 Oct ·
> Week 3 = 26 Oct – 1 Nov (code freeze Thu 29 Oct). Docker is added by Phat **after** Week 3.

---

## 0. Status & re-plan (as of Tue 27 Oct 2026)

> Sections 3–5 are the original plan and are kept as written. **This section is the current plan.**
> The proposed dates below need a quick "ok" from all three in the group chat.

### 0.1 Where we are

| State | Tasks |
|---|---|
| ✅ Done (on `main`) | W1-01 repo · W1-02 skeleton · W1-03 design review (`common.h`) · W1-04 `setup.sh` + README · W1-05 CI (plain/TSan/ASan) · W1-06 `gen_input.sh` + `verify.sh` · W1-08 queue (#1) · W2-06 **interface only**: `metrics.h` proposal (#2) |
| 🔍 In review | W1-09 pool (#5, Son): **plain CI job failing**, no approval yet · W3-01 sweep (#3, Son): CI green, needs review · W3-02 plots (#4, Son): CI green, needs review |
| 🧱 Stub on `main`, not started | W1-07 `io.c` (Vy) · W1-10 `baseline.c` (Vy) · W2-01 `pipeline.c` (Phat) |
| ⏳ Not started | W1-11 · W2-02 faults · W2-03 ratelimit · W2-04 cancel · W2-05 supervisor · W2-06 `metrics.c` · W2-07 stdin · W2-08 mmap · W2-09 unbounded · W2-10 e2e · W2-11 memory test · W3-03…W3-09 |

**The program can't copy a file yet.** Neither `--mode=blocking` nor `--mode=pipeline` works.
Most of Sprint 1 and all of Sprint 2 have slipped into Week 3.

### 0.2 Critical path

| Task | Waits for | Unblocks |
|---|---|---|
| W1-07 `io.c` | — | W1-10, W2-01, W2-02, W2-03, W2-07, W2-08 |
| W1-09 pool (#5) | — | W2-01, W2-09 |
| W1-10 baseline | W1-07 | W1-11, W3-03 |
| W2-01 pipeline | W1-07, W1-09 | W2-04, W2-05, W2-06 wiring, W2-10, W3-01 real runs |
| W2-02 faults · W2-03 ratelimit | W1-07 | W2-10 |
| W2-04 cancel · W2-05 supervisor | W2-01 | W2-10 |
| W2-10 e2e matrix | W2-01…W2-05, W2-08 | W3-05 `v1.0` |
| W3-01/02 on real binary | W2-01, W2-06 | W3-06 analysis → W3-07 report |

**`io.c` (W1-07) and the pool (#5) block everyone. Both must be merged on Tue 27 or Wed 28 at the latest.**

### 0.3 Scope cut (to fit the remaining days)

| Keep: acceptance gate needs it | Should: do if on time | Cut / after 1 Nov |
|---|---|---|
| W1-07, W1-09, W1-10, W2-01 (gate #2 needs a pipeline) | W2-07 nonblocking stdin + `poll` | W3-04 stretch (zero-copy / `pwritev`) |
| W2-02 faults (short I/O, EINTR) · W2-03 `--out-rate` (slow consumer) | W2-11 RSS assertion test | Docker (already planned for after 1 Nov) |
| W2-04 Ctrl+C · W2-05 `--fail-worker` (simple version: replacement + requeue) | `--fault-eagain` / `--fault-delay-us` cases | |
| W2-06 `metrics.c` · W2-09 `--unbounded` (memory blow-up demo) | blocking vs nonblocking comparison in W3-03 | |
| W2-08 `--io=mmap` (the brief needs ≥2 I/O strategies: rw vs mmap) | | |
| W2-10 e2e matrix (short, EINTR, slow output, fail-worker, SIGINT) | | |
| W3-01/02 sweeps + graphs · W3-05 clean VM · W3-06…09 | | |

### 0.4 Day-by-day plan (proposed)

**Proposed:** move the **code freeze from Thu 29 to the end of Fri 30 Oct**, so Saturday is for
measuring and Sunday is for the report and slides. If the lecturer's hard deadline (§8) is later than
1 Nov, use the extra days for the report instead of more features.

| Day | Vy | Son | Phat |
|---|---|---|---|
| **Tue 27** | **W1-07 `io.c` + `test_io.c` → PR today** | Fix the failing plain CI job on #5, merge the pool · address review on #3/#4 | Review #3, #4, #5 (and #5's `pool_destroy` design entry) · draft W2-01 against `queue.h`/`pool.h`/`io.h` |
| **Wed 28** | W1-10 baseline · W2-02 faults (short + EINTR first) | W2-06 `metrics.c` + `test_metrics.c` · W2-09 `--unbounded` | Review `io.c` · W2-01 pipeline merged (reader, workers, writer, bitmap, shutdown order) · W1-11 baseline e2e in CI |
| **Thu 29** | W2-03 ratelimit · W2-08 `--io=mmap` | W2-04 cancel (sigwait, drain, exit 130) | W2-05 supervisor (simple version) · start `tests/e2e.sh` |
| **Fri 30** | W2-07 stdin `poll` (should) · fix e2e failures in `io.c`/faults | Help Phat with e2e cases (SIGINT, slow output) · W2-11 RSS test (taken over from Phat) | W2-10 e2e matrix green in CI · wire metrics calls in the pipeline · **🔒 code freeze 23:59** |
| **Sat 31** | W3-03 rw vs mmap (cold/warm) | W3-01 sweep on the real binary · W3-02 graphs | W3-05 clean-VM run, tag `v1.0` · W3-06 analysis together (evening) |
| **Sun 1** | W3-07 report (own module) · W3-08 knowledge share · W3-09 slides + rehearsal | same | same |

**Daily check:** by 22:00, everyone posts what got merged. A blocking PR (io, pool, pipeline) gets
reviewed within 2 hours of being opened.

### 0.5 Open items found in the 27 Oct review

- `docs/design.md` W2-06 entry still says *"proposed, pending Phat and Vy re-review"*, but #2 is
  merged. Son: update the status line (the gate allows `metrics.c` to start).
- `docs/design.md` W1-09 `pool_destroy` entry is *"Pending"*. It needs all-three approval on #5.
- Overview §10 shows the repo root as `io-pipeline/`; the repo is `pipe-chunk/`. Cosmetic.
- No GitHub issues exist (`gh issue list` is empty), so PRs can't follow §2's `Closes #<issue>`
  step. Until the team decides otherwise, a PR names its task ID in the title (e.g.
  `W1-07 io.c read_full/pwrite_full`) and the board tracks the PR instead of an issue.
- §8 hard deadline and report format are still unconfirmed. **Ask the lecturer this week.** The
  proposed freeze move depends on it.

---

## 1. Team & roles

| Member | Role | Owns (code + tests + docs for these) |
|---|---|---|
| **Phat** | Integration + testing | Repo/board setup, `setup.sh`, README, **CI** (GitHub Actions), `verify.sh`, **e2e/fault test suite**, **pipeline wiring** (starting/connecting/joining threads, shutdown order, writer), **worker-failure supervisor**, reviewing PRs, releases |
| **Vy** | I/O engineer | `io.c` (read_full, pwrite_full, nonblocking + `poll`, `mmap`), `faults.c` (fault injection), `ratelimit.c`, `baseline.c` (blocking mode), stretch goal (zero-copy / vectored) |
| **Son** | Concurrency core + evaluation | `queue.c` (bounded queue), `pool.c` (buffer pool), `cancel.c` (Ctrl+C), `metrics.c` (timers, monitor thread, CSV), `--unbounded` mode, benchmarks + graphs |
| **Everyone** | — | Unit tests for own module, report section for own module, explain *all* concepts in the final presentation |

**Testing split:** module owners write **unit tests** for their own module (`tests/test_<module>.c`).
Phat writes the **end-to-end and fault-injection test cases** and makes CI run them.

All three are equal team members with no project manager; decisions are made together at the Monday meeting.

**Why this split:** Phat's work (CI, tests, pipeline) touches everyone's module, so it makes sense for
one person to own integration. Vy's work is all "talking to the kernel", and Son's is all "threads sharing data"
plus measuring it. Each person can work in parallel once the interfaces in Overview §10 are agreed.

---

## 2. How we work on GitHub

### Board & issues
- **GitHub Project board** columns: `Backlog → To Do → In Progress → In Review → Done`.
- **Milestones:** `Sprint 1`, `Sprint 2`, `Sprint 3`.
- **Labels:** `core`, `io`, `test`, `ci`, `metrics`, `docs`, `bug`, `stretch`.
- Every task below is one issue. Issue title = task ID + name, e.g. `W1-08 Bounded queue`.

### Branch → PR → merge
1. Take an issue, assign yourself, move to *In Progress*.
2. Branch: `feat/<task-id>-<short-name>` (e.g. `feat/w1-08-queue`).
3. Small commits, imperative messages: `Add close() to bounded queue`.
4. Open a PR with `Closes #<issue>` in the description.
5. **Merge rules on `main`** (team rule, not enforced by GitHub): PR required · 1 approval · CI
   green · no direct pushes. A change to any agreed header in Overview §10 requires approval from
   all three members and a note in `docs/design.md`.
6. Squash-merge, delete the branch.

**Reviewer rotation:** Vy's PRs → Son reviews · Son's PRs → Vy reviews · Phat's PRs → either. Phat
also skims every PR touching `pipeline.c` or `tests/`.

### Definition of Done (for every task)
- [ ] Code merged to `main`, CI green (build + unit tests + TSan + ASan)
- [ ] Has a test that would fail if the logic broke
- [ ] Short note in `docs/design.md` if it changes behavior/interfaces
- [ ] Issue closed

### Meetings
| When | What |
|---|---|
| **Monday meeting** | Review last week (demo what works, what slipped) · plan this week's issues and owners · fix blockers |
| **Daily, group chat** | Each person posts: *Yesterday I did… / Today I'll do… / Blocked on…* |

---

## 3. Sprint 1 — "Foundation: it works, slowly but correctly" (12–18 Oct)

**Goal:** shared skeleton, CI running, a correct baseline, and the two core data structures done.

**Problems this sprint solves**
- Three people can't code in parallel without a shared skeleton and agreed interfaces → **skeleton + headers first**.
- "Works on my machine" fails the acceptance gate → **setup.sh + CI from day 1**.
- Race conditions found in week 3 are very expensive → **ThreadSanitizer in CI from day 1**.
- We need an objective proof of "no loss, no duplication" → **verify.sh**.
- The brief requires a blocking baseline to compare against → **baseline mode**.

| ID | Task | Owner | Due | Depends on | Problem it solves |
|---|---|---|---|---|---|
| W1-01 | Repo, board, labels, milestones | Phat | Mon | — | Team needs one place to track work |
| W1-02 | Skeleton: folder layout, `Makefile`, header files with the agreed interfaces (stubs that compile) | Phat | Tue | W1-01, W1-03 | Lets Vy & Son code in parallel against fixed contracts |
| W1-03 | **Design review (at the Monday meeting):** record struct, ownership table, sync table, interfaces (Overview §5, §10) | All | Mon | — | Everyone agrees *who owns which buffer when* before writing code |
| W1-04 | `scripts/setup.sh` + README v1 (install deps, build, run) | Phat | Wed | W1-02 | Clean-machine reproduction (acceptance gate #1) |
| W1-05 | CI: `.github/workflows/ci.yml` — build, unit tests, TSan build, ASan build on every PR | Phat | Wed | W1-02 | Catches races and memory bugs automatically on every change |
| W1-06 | `gen_input.sh` (test files) + `verify.sh` (SHA compare + manifest exactly-once check) | Phat | Thu | W1-02 | Objective correctness proof used by every later test |
| W1-07 | `io.c`: `read_full` / `pwrite_full` handling short counts, `EINTR`, EOF + `test_io.c` | Vy | Thu | W1-02 | Partial I/O & interrupted calls (brief requirement) |
| W1-08 | `queue.c`: ring buffer, mutex, `not_full`/`not_empty`, `close()` + stress test (many producers/consumers) | Son | Thu | W1-02 | Producer–consumer synchronization; the base of backpressure |
| W1-09 | `pool.c`: fixed tray pool, blocking `get`, `put`, `close` + test | Son | Fri | W1-08 | Bounded memory + clear buffer ownership |
| W1-10 | `baseline.c`: `--mode=blocking` (read → SHA-256 → pwrite → manifest) | Vy | Sat | W1-07 | Required baseline; reference for correctness and speed |
| W1-11 | CI job: run baseline on a 100 MB file + `verify.sh` | Phat | Sun | W1-05, W1-06, W1-10 | First end-to-end proof in CI |

**Sprint 1 exit criteria:** baseline copies a 1 GB file and `verify.sh` passes · queue & pool stress tests
pass under TSan · CI green on `main` · `setup.sh` works on a teammate's machine.

---

## 4. Sprint 2 — "Concurrency & robustness: fast, and survives chaos" (19–25 Oct)

**Goal:** the concurrent pipeline with backpressure, surviving every failure the brief lists.

**Problems this sprint solves**
- Baseline uses one core and waits on I/O → **concurrent pipeline**.
- Fast input + slow output would grow memory → **backpressure via bounded queues + pool**.
- Real syscalls rarely misbehave on demand → **fault injection** to force short I/O, `EINTR`, `EAGAIN`, delays.
- A crashing stage could lose or duplicate records → **supervisor + requeue + idempotent pwrite + writer bitmap**.
- Ctrl+C could leave half-written output → **graceful drain cancellation**.
- We need data for the evaluation → **metrics + rate limiter**.
- Brief requires two I/O strategies and nonblocking behavior → **mmap + nonblocking pipe input**.

| ID | Task | Owner | Due | Depends on | Problem it solves |
|---|---|---|---|---|---|
| W2-01 | `pipeline.c`: start reader, N workers, writer; wire Q1/Q2 + pool; EOF shutdown order; writer `pwrite` at offset + seq bitmap | Phat | Tue | W1-08, W1-09, W1-07 | The required concurrent version; correct shutdown |
| W2-02 | `faults.c`: wrappers injecting short I/O, `EINTR`, `EAGAIN`, delays; `--seed` for reproducibility | Vy | Tue | W1-07 | Lets us *prove* robustness to partial/interrupted I/O |
| W2-03 | `ratelimit.c`: token bucket for `--in-rate` / `--out-rate` | Vy | Wed | W1-07 | Simulates slow consumer / fast producer for tests and experiments |
| W2-04 | `cancel.c`: signal thread with `sigwait`, graceful drain, second Ctrl+C = hard stop, exit code 130 | Son | Wed | W2-01 | Clean cancellation without losing accepted records |
| W2-05 | **Worker-failure supervisor:** in-flight slots, `--fail-worker=P`, join dead worker, respawn first, requeue, `MAX_ATTEMPTS` | Phat | Thu | W2-01 | Stage failure without loss or duplication (acceptance gate #2) |
| W2-06 | `metrics.c`: latency timestamps (p50/p99), monitor thread sampling occupancy every 10 ms, `getrusage`, I/O-wait timers, CSV output | Son | Thu | W2-01 | All required measurements |
| W2-07 | Nonblocking stdin pipe input: `O_NONBLOCK` + `poll()`, `EAGAIN` path | Vy | Fri | W2-02 | Blocking vs nonblocking; "temporary unavailability" |
| W2-08 | `--io=mmap` input strategy | Vy | Sat | W2-01 | Second I/O strategy for the comparison |
| W2-09 | `--unbounded` mode in queue + pool | Son | Fri | W1-08, W1-09 | Demonstrates memory blow-up without flow control |
| W2-10 | **e2e fault matrix** (`tests/e2e.sh`), run in CI, each case checked by `verify.sh`: short I/O · EINTR · EAGAIN · slow output (`--out-rate`) · `--fail-worker` · SIGINT mid-run · pipe input · mmap | Phat | Sun | W2-02…W2-08 | Acceptance gate #2, enforced automatically on every PR |
| W2-11 | Peak-memory assertion test: slow output, bounded mode → RSS stays under limit | Phat | Sun | W2-06 | Proves backpressure keeps memory bounded |

**Sprint 2 exit criteria:** every e2e fault case passes in CI · TSan reports no races · memory stays
bounded with a slow writer · `--unbounded` visibly grows · metrics CSVs are produced.

---

## 5. Sprint 3 — "Measure, explain, deliver" (26 Oct – 1 Nov)

**Goal:** evaluation results, a reproducible release, a report, and a team that can explain everything.

**Problems this sprint solves**
- The brief requires varying buffer size and worker count and explaining the trade-off → **benchmark sweeps + analysis**.
- The brief requires comparing two I/O strategies → **rw vs mmap (and pipe/nonblocking) comparison**.
- A clean-machine reproduction must work with no hidden steps → **fresh-VM run**.
- Every member must explain all concepts → **knowledge-share session**.

| ID | Task | Owner | Due | Depends on | Problem it solves |
|---|---|---|---|---|---|
| W3-01 | `bench/sweep.sh`: queue {1,4,16,64} × workers {1,2,4,8} × block {4K,64K,1M}, 5 runs each, median | Son | Mon | W2-06 | Required parameter variation |
| W3-02 | `bench/plot.py`: throughput vs workers, latency vs queue size, occupancy over time, memory bounded vs unbounded | Son | Tue | W3-01 | Turns data into evidence for the report |
| W3-03 | Strategy comparison: `rw` vs `mmap` (cold and warm cache), blocking vs nonblocking pipe input | Vy | Tue | W2-07, W2-08 | Required I/O strategy comparison |
| W3-04 | Stretch: `pwritev` batching **or** `copy_file_range` zero-copy path + measurement | Vy | Wed | W3-03 | Stretch goal (only if W3-03 is done) |
| W3-05 | Clean-VM test: fresh Ubuntu 24.04 VM, follow README only, fix every gap; tag `v1.0` | Phat | Wed | all W2 | Acceptance gate #1 |
| W3-06 | Analysis (in group chat / shared doc): explain each graph (why throughput plateaus, why latency rises with queue size) | All | Wed | W3-02, W3-03 | Acceptance gate #3 |
| — | 🔒 **Code freeze** — bug fixes only | — | **Thu** | — | Stops last-minute breakage |
| W3-07 | Report: each person writes own module + results section; intro, design and conclusion split at the Monday meeting | All | Fri | W3-06 | Final deliverable |
| W3-08 | Knowledge-share: each person explains the *other two* members' modules + the 5 core concepts | All | Sat | — | Acceptance gate #4 |
| W3-09 | Slides + demo script (live: slow output → backpressure; `--fail-worker`; Ctrl+C) + rehearsal | All | Sun | W3-07 | Presentation |
| (after) | Docker image + run script | Phat | after 1 Nov | v1.0 | Containerized reproduction |

**Sprint 3 exit criteria:** `v1.0` tagged · README reproduces on a fresh VM · graphs + written
trade-off explanation · report and slides done · everyone passes the knowledge-share.

---

## 6. Requirement → task traceability

| Brief requirement | Tasks |
|---|---|
| Baseline blocking version | W1-07, W1-10 |
| Concurrent version with bounded queues, ownership & sync defined | W1-03, W1-08, W1-09, W2-01 |
| Short reads/writes, EOF, interrupted ops | W1-07, W2-02, W2-10 |
| Temporary unavailability | W2-02, W2-07 |
| Worker failure without loss/duplication | W2-05, W2-10 |
| Clean cancellation | W2-04, W2-10 |
| Backpressure / bounded memory | W1-08, W1-09, W2-09, W2-11 |
| Metrics: throughput, latency, occupancy, CPU, memory, I/O wait | W2-06 |
| Vary buffer size, worker count, I/O speed | W2-03, W3-01, W3-02 |
| Compare ≥2 I/O strategies | W2-07, W2-08, W3-03 |
| Stretch: zero-copy / vectored I/O | W3-04 |
| Clean-machine reproduction | W1-04, W1-05, W3-05 (+ Docker later) |
| Everyone can explain the concepts | W1-03, W3-06, W3-08 |

---

## 7. Risks & mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Interfaces change mid-sprint | Merge conflicts, broken builds | Interfaces frozen after W1-03; changes need a PR approved by all 3 |
| Race conditions / deadlocks | Random failures, wrong output | TSan in CI; ownership table; `bq_close` broadcast tested |
| Phat overloaded (CI + tests + pipeline + supervisor) | Sprint 2 slips | W2-05 can start with a simple version; Son helps with e2e cases if W2-04/06 finish early |
| Page cache hides disk cost | Misleading numbers | Use files > RAM or drop caches (`sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'`) natively |
| Noisy benchmarks | Unclear graphs | 5 runs, median, same machine, close other apps |
| Stretch goal eats time | Core work late | Stretch only starts after W3-03 is merged |
| Someone unavailable for days | Tasks blocked | Daily group-chat check-in flags it early; re-assign in chat or at Monday meeting |

---

## 8. Open questions (team to confirm)

- [x] Start date confirmed: Sprint 1 starts Mon 12 Oct 2026. Hard deadline still to confirm.
- [ ] Final deliverables: report page limit? slides? live demo? individual contribution statement?
- [ ] Does the lecturer require a specific report format or a git history review?
- [x] OpenSSL (`libcrypto`) is allowed → we use its SHA-256 for block checksums.
