#!/usr/bin/env bash
# sweep.sh [BINARY] — parameter sweep: queue size × workers × block size, RUNS runs each, median.
# BINARY defaults to build/pipe-chunk. Every run is checked with verify.sh; a failed run stops the sweep.
#
# Settings come from the environment (defaults in brackets):
#   SIZE     input file size, head -c suffixes [1G]
#   RUNS     runs per configuration [5]
#   QUEUES   queue capacities [1 4 16 64]
#   WORKERS  worker counts [1 2 4 8]
#   BLOCKS   block sizes, K/M suffixes [4K 64K 1M]
#   OUT      results directory [bench/results]
#   WORK     where the input and output copies live [/dev/shm if it exists, else OUT]
#
# The blocking baseline is also run once per block size and round, as the single-threaded reference.
# Runs go round-robin (round 1 of every config, then round 2, ...) so slow drift on the machine
# spreads over all configs instead of biasing one. One untimed warm-up copy runs first.
# Input and output live in RAM (tmpfs) by default, so the sweep measures CPU and synchronization
# costs, not the disk: on an SSD the final fsync of each 1 GiB copy made the same config vary 2-3x
# between runs and capped every config at the drive's sustained write speed. Set WORK to a disk
# directory to measure with the disk included (needs 2 x SIZE free there).
#
# Output in OUT:
#   sweep_raw.csv   one row per run: mode,queue,workers,block,run + the --metrics summary columns
#   sweep.csv       one row per config: medians, plus min/max throughput over the runs
#   occupancy/      PREFIX.occupancy.csv of round 1 for each pipeline config (queue sizes every 10 ms)
#   sweep_env.txt   machine, kernel, compiler, commit and settings, for reproducing the numbers
set -euo pipefail
# awk parses "0.017" as 0 under a locale with a decimal comma (e.g. LC_NUMERIC=vi_VN).
export LC_ALL=C
cd "$(dirname "$0")/.."
bin=${1:-build/pipe-chunk}
[ -x "$bin" ] || { echo "sweep: $bin not found, run make first" >&2; exit 2; }

SIZE=${SIZE:-1G}
RUNS=${RUNS:-5}
QUEUES=${QUEUES:-1 4 16 64}
WORKERS=${WORKERS:-1 2 4 8}
BLOCKS=${BLOCKS:-4K 64K 1M}
OUT=${OUT:-bench/results}
if [ -z "${WORK:-}" ]; then
    if [ -d /dev/shm ] && [ -w /dev/shm ]; then WORK=/dev/shm; else WORK=$OUT; fi
fi

fail() { echo "sweep FAIL: $*" >&2; exit 1; }

# to_bytes 64K -> 65536
to_bytes() {
    local n=${1%[KkMm]}
    case $1 in
        *[Kk]) echo $((n << 10)) ;;
        *[Mm]) echo $((n << 20)) ;;
        *)     echo "$n" ;;
    esac
}

mkdir -p "$OUT/occupancy"
work=$(mktemp -d "$WORK/pipe-chunk-sweep.XXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 130' INT TERM

# Configs: "mode queue workers block_bytes block_label"
configs=()
for b in $BLOCKS; do
    configs+=("blocking - - $(to_bytes "$b") $b")
    for q in $QUEUES; do
        for w in $WORKERS; do
            configs+=("pipeline $q $w $(to_bytes "$b") $b")
        done
    done
done

{
    echo "date: $(date -Iseconds)"
    echo "commit: $(git rev-parse --short HEAD 2>/dev/null || echo unknown)$(git diff --quiet 2>/dev/null || echo ' (dirty)')"
    echo "cpu: $(lscpu | sed -n 's/^Model name: *//p'), $(nproc) online"
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
    echo "memory: $(free -h | awk '/^Mem:/ { print $2 }')"
    echo "kernel: $(uname -r)"
    echo "compiler: $(${CC:-cc} --version | head -1)"
    echo "filesystem: $(df -T "$work" | awk 'NR == 2 { print $2 " on " $1 }')"
    echo "settings: SIZE=$SIZE RUNS=$RUNS WORK=$WORK QUEUES='$QUEUES' WORKERS='$WORKERS' BLOCKS='$BLOCKS'"
} > "$OUT/sweep_env.txt"

./scripts/gen_input.sh "$SIZE" "$work/in.bin"
"$bin" --metrics="$work/m" "$work/in.bin" "$work/out.bin" 2>/dev/null || fail "warm-up copy: exit $?"

raw=$OUT/sweep_raw.csv
echo "mode,queue,workers,block,run,$(head -1 "$work/m.summary.csv")" > "$raw"

total=$(( ${#configs[@]} * RUNS )) i=0
for run in $(seq 1 "$RUNS"); do
    for c in "${configs[@]}"; do
        read -r mode q w bytes label <<< "$c"
        i=$((i + 1))
        if [ "$mode" = blocking ]; then
            args=(--mode=blocking -b "$bytes"); qcol= wcol= name="blocking_b$label"
        else
            args=(-q "$q" -w "$w" -b "$bytes"); qcol=$q wcol=$w name="pipeline_q${q}_w${w}_b$label"
        fi
        rm -f "$work/out.bin" "$work/out.bin.manifest"
        rc=0
        "$bin" "${args[@]}" --metrics="$work/m" "$work/in.bin" "$work/out.bin" 2>"$work/err" || rc=$?
        [ "$rc" -ne 130 ] || { echo "sweep: interrupted" >&2; exit 130; }   # Ctrl+C reached the copy
        [ "$rc" -eq 0 ] || fail "$name run $run: exit $rc ($(cat "$work/err"))"
        ./scripts/verify.sh "$work/in.bin" "$work/out.bin" >/dev/null || fail "$name run $run"
        echo "$mode,$qcol,$wcol,$bytes,$run,$(tail -1 "$work/m.summary.csv")" >> "$raw"
        if [ "$run" -eq 1 ] && [ "$mode" = pipeline ]; then
            cp "$work/m.occupancy.csv" "$OUT/occupancy/$name.csv"
        fi
        printf 'sweep [%d/%d] %s run %d: %s MB/s\n' "$i" "$total" "$name" "$run" \
            "$(tail -1 "$work/m.summary.csv" | cut -d, -f4)"
    done
done

awk -v keys=4 -f bench/median.awk "$raw" > "$OUT/sweep.csv"

echo "sweep ok: $total runs, medians in $OUT/sweep.csv"
