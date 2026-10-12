#!/usr/bin/env bash
# github_bootstrap.sh OWNER/REPO — one-time GitHub setup for the plan in docs/02-PLAN-AND-ROLES.md:
#   labels, 3 sprint milestones, one issue per task, a "pipe-chunk" project board, branch protection on main.
# Safe to re-run: anything that already exists is skipped.
# Needs the GitHub CLI: https://cli.github.com, then `gh auth login` and `gh auth refresh -s project`.
set -euo pipefail
[ $# -eq 1 ] || { echo "usage: $0 OWNER/REPO" >&2; exit 2; }
repo=$1; owner=${repo%%/*}

# Sprint end dates (Sunday). Shift these if the team moves the schedule.
declare -A due=( [1]=2026-10-18 [2]=2026-10-25 [3]=2026-11-01 )
declare -A goal=(
    [1]="Foundation: skeleton, CI, correct baseline, queue + pool"
    [2]="Concurrency & robustness: pipeline with backpressure, survives every fault"
    [3]="Measure, explain, deliver: benchmarks, graphs, v1.0, report, slides"
)

echo "== labels"
while IFS='|' read -r name color desc; do
    gh label create "$name" -R "$repo" --color "$color" --description "$desc" --force >/dev/null
done <<'EOF'
core|1d76db|Pipeline, queue, pool, cancellation
io|0e8a16|Talking to the kernel: read/write, poll, mmap, faults
test|fbca04|Unit, e2e and fault tests
ci|5319e7|GitHub Actions, build, setup
metrics|c5def5|Measurements, benchmarks, graphs
docs|0075ca|Docs, report, slides
bug|d73a4a|Something is broken
stretch|bfdadc|Only after the core work is merged
EOF

echo "== milestones"
existing=$(gh api "repos/$repo/milestones?state=all&per_page=100" --jq '.[].title')
for s in 1 2 3; do
    grep -qx "Sprint $s" <<<"$existing" && continue
    gh api "repos/$repo/milestones" -f title="Sprint $s" -f description="${goal[$s]}" \
        -f due_on="${due[$s]}T23:59:59Z" >/dev/null
done

echo "== project board"
proj=$(gh project list --owner "$owner" --format json --jq '.projects[] | select(.title=="pipe-chunk") | .number')
[ -n "$proj" ] || proj=$(gh project create --owner "$owner" --title pipe-chunk --format json --jq .number)

echo "== issues"
# id | sprint | labels | title | owner | due | depends on | problem it solves
while IFS='|' read -r id s labels title who when deps problem; do
    [ -n "$id" ] || continue
    url=$(gh issue list -R "$repo" --state all --search "$id in:title" --json title,url \
          --jq ".[] | select(.title | startswith(\"$id \")) | .url" | head -1)
    if [ -z "$url" ]; then
        body="**Owner:** $who · **Due:** $when (Sprint $s) · **Depends on:** $deps

**Problem it solves:** $problem

Details: [plan](https://github.com/$repo/blob/main/docs/02-PLAN-AND-ROLES.md) · [overview](https://github.com/$repo/blob/main/docs/01-PROJECT-OVERVIEW.md)

**Definition of Done**
- [ ] Code merged to \`main\`, CI green (build + unit tests + TSan + ASan)
- [ ] Has a test that would fail if the logic broke
- [ ] Short note in \`docs/design.md\` if it changes behavior/interfaces"
        url=$(gh issue create -R "$repo" --title "$id $title" --body "$body" \
              --label "$labels" --milestone "Sprint $s")
        echo "created $id -> $url"
    else
        echo "exists  $id"
    fi
    gh project item-add "$proj" --owner "$owner" --url "$url" >/dev/null
done <<'EOF'
W1-01|1|ci|Repo, board, labels, milestones, branch protection|Phat|Mon|—|Team needs one place to track work and a protected main
W1-02|1|core|Skeleton: folder layout, Makefile, headers with agreed interfaces|Phat|Tue|W1-01, W1-03|Lets Vy & Son code in parallel against fixed contracts
W1-03|1|docs|Design review: record struct, ownership table, sync table, interfaces|All|Mon|—|Everyone agrees who owns which buffer when before writing code
W1-04|1|ci,docs|setup.sh + README v1|Phat|Wed|W1-02|Clean-machine reproduction (acceptance gate #1)
W1-05|1|ci|CI: build, unit tests, TSan build, ASan build on every PR|Phat|Wed|W1-02|Catches races and memory bugs automatically on every change
W1-06|1|test|gen_input.sh + verify.sh (SHA compare + manifest exactly-once)|Phat|Thu|W1-02|Objective correctness proof used by every later test
W1-07|1|io|io.c: read_full / pwrite_full (short counts, EINTR, EOF) + test_io.c|Vy|Thu|W1-02|Partial I/O & interrupted calls (brief requirement)
W1-08|1|core|Bounded queue (queue.c) + many-producer/consumer stress test|Son|Thu|W1-02|Producer-consumer synchronization; the base of backpressure
W1-09|1|core|Buffer pool (pool.c): fixed trays, blocking get, put, close + test|Son|Fri|W1-08|Bounded memory + clear buffer ownership
W1-10|1|io|Baseline --mode=blocking (baseline.c)|Vy|Sat|W1-07|Required baseline; reference for correctness and speed
W1-11|1|ci,test|CI job: baseline on a 100 MB file + verify.sh|Phat|Sun|W1-05, W1-06, W1-10|First end-to-end proof in CI
W2-01|2|core|pipeline.c: reader, N workers, writer, Q1/Q2 + pool, EOF shutdown, writer bitmap|Phat|Tue|W1-07, W1-08, W1-09|The required concurrent version; correct shutdown
W2-02|2|io,test|faults.c: inject short I/O, EINTR, EAGAIN, delays; --seed|Vy|Tue|W1-07|Lets us prove robustness to partial/interrupted I/O
W2-03|2|io|ratelimit.c: token bucket for --in-rate / --out-rate|Vy|Wed|W1-07|Simulates slow consumer / fast producer for tests and experiments
W2-04|2|core|cancel.c: sigwait thread, graceful drain, 2nd Ctrl+C hard stop, exit 130|Son|Wed|W2-01|Clean cancellation without losing accepted records
W2-05|2|core|Worker-failure supervisor: in-flight slots, --fail-worker, respawn, requeue, MAX_ATTEMPTS|Phat|Thu|W2-01|Stage failure without loss or duplication (acceptance gate #2)
W2-06|2|metrics|metrics.c: latency p50/p99, occupancy monitor, getrusage, I/O-wait, CSV|Son|Thu|W2-01|All required measurements
W2-07|2|io|Nonblocking stdin pipe input: O_NONBLOCK + poll(), EAGAIN path|Vy|Fri|W2-02|Blocking vs nonblocking; temporary unavailability
W2-08|2|io|--io=mmap input strategy|Vy|Sat|W2-01|Second I/O strategy for the comparison
W2-09|2|core|--unbounded mode in queue + pool|Son|Fri|W1-08, W1-09|Demonstrates memory blow-up without flow control
W2-10|2|test,ci|e2e fault matrix (tests/e2e.sh) in CI, every case checked by verify.sh|Phat|Sun|W2-02 … W2-08|Acceptance gate #2, enforced automatically on every PR
W2-11|2|test|Peak-memory assertion: slow output, bounded mode, RSS stays under limit|Phat|Sun|W2-06|Proves backpressure keeps memory bounded
W3-01|3|metrics|bench/sweep.sh: queue x workers x block, 5 runs, median|Son|Mon|W2-06|Required parameter variation
W3-02|3|metrics|bench/plot.py: throughput, latency, occupancy, memory graphs|Son|Tue|W3-01|Turns data into evidence for the report
W3-03|3|io,metrics|Strategy comparison: rw vs mmap (cold/warm), blocking vs nonblocking pipe|Vy|Tue|W2-07, W2-08|Required I/O strategy comparison
W3-04|3|io,stretch|Stretch: pwritev batching or copy_file_range zero-copy + measurement|Vy|Wed|W3-03|Stretch goal (only if W3-03 is done)
W3-05|3|ci|Clean-VM test on fresh Ubuntu 24.04, fix gaps, tag v1.0|Phat|Wed|all W2|Acceptance gate #1
W3-06|3|docs|Analysis: explain each graph and the throughput/latency trade-off|All|Wed|W3-02, W3-03|Acceptance gate #3
W3-07|3|docs|Report: own module + results; intro/design/conclusion split|All|Fri|W3-06|Final deliverable
W3-08|3|docs|Knowledge-share: each person explains the other two modules + 5 core concepts|All|Sat|—|Acceptance gate #4
W3-09|3|docs|Slides + demo script (backpressure, --fail-worker, Ctrl+C) + rehearsal|All|Sun|W3-07|Presentation
EOF

echo "== branch protection on main (needs main pushed; private repos need a paid plan)"
gh api -X PUT "repos/$repo/branches/main/protection" --input - >/dev/null <<'EOF' && echo ok || echo "skipped: set it by hand in Settings > Branches"
{
  "required_status_checks": { "strict": false,
    "contexts": ["build+test plain", "build+test thread", "build+test address"] },
  "enforce_admins": false,
  "required_pull_request_reviews": { "required_approving_review_count": 1 },
  "restrictions": null
}
EOF

echo "done: https://github.com/$repo/issues · board: gh project view $proj --owner $owner --web"
