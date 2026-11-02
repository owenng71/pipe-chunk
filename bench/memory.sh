#!/usr/bin/env bash
# memory.sh [BINARY] — backpressure experiment: peak memory with a slow writer, bounded vs --unbounded.
# BINARY defaults to build/pipe-chunk. Every run is checked with verify.sh; a failed run stops it.
#
# Settings come from the environment (defaults in brackets):
#   SIZES     input sizes, head -c suffixes [64M 128M 256M 512M]
#   RUNS      runs per configuration [3]
#   OUT_RATE  writer limit in MB/s, far below what the reader and workers manage [200]
#   OUT       results directory [bench/results]
#   WORK      where the copies live [/dev/shm if it exists, else OUT]
#
# Fixed settings: -q 4 -w 4 -b 64K. Bounded mode can hold at most 2*4 + 4 + 2 = 14 blocks, so its
# peak memory should not depend on the file size; --unbounded keeps reading while the writer falls
# behind, so it should buffer most of the file.
#
# Output in OUT:
#   memory_raw.csv  one row per run: mode,size,run + the --metrics summary columns
#   memory.csv      one row per config: median peak RSS and throughput
#   occupancy/memory_{bounded,unbounded}.csv  queue sizes every 10 ms, largest size, round 1
set -euo pipefail
# awk parses "0.017" as 0 under a locale with a decimal comma (e.g. LC_NUMERIC=vi_VN).
export LC_ALL=C
cd "$(dirname "$0")/.."
bin=${1:-build/pipe-chunk}
[ -x "$bin" ] || { echo "memory: $bin not found, run make first" >&2; exit 2; }

SIZES=${SIZES:-64M 128M 256M 512M}
RUNS=${RUNS:-3}
OUT_RATE=${OUT_RATE:-200}
OUT=${OUT:-bench/results}
if [ -z "${WORK:-}" ]; then
    if [ -d /dev/shm ] && [ -w /dev/shm ]; then WORK=/dev/shm; else WORK=$OUT; fi
fi

fail() { echo "memory FAIL: $*" >&2; exit 1; }

mkdir -p "$OUT/occupancy"
work=$(mktemp -d "$WORK/pipe-chunk-memory.XXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 130' INT TERM

largest=${SIZES##* }
raw=$OUT/memory_raw.csv
header=
for size in $SIZES; do
    ./scripts/gen_input.sh "$size" "$work/in.bin"
    bytes=$(stat -c %s "$work/in.bin")
    for run in $(seq 1 "$RUNS"); do
        for mode in bounded unbounded; do
            args=(-q 4 -w 4 -b 64K --out-rate="$OUT_RATE")
            [ "$mode" = bounded ] || args+=(--unbounded)
            rm -f "$work/out.bin" "$work/out.bin.manifest"
            rc=0
            "$bin" "${args[@]}" --metrics="$work/m" "$work/in.bin" "$work/out.bin" 2>"$work/err" || rc=$?
            [ "$rc" -ne 130 ] || { echo "memory: interrupted" >&2; exit 130; }
            [ "$rc" -eq 0 ] || fail "$mode $size run $run: exit $rc ($(cat "$work/err"))"
            ./scripts/verify.sh "$work/in.bin" "$work/out.bin" >/dev/null || fail "$mode $size run $run"
            if [ -z "$header" ]; then
                header=$(head -1 "$work/m.summary.csv")
                echo "mode,size,run,$header" > "$raw"
            fi
            echo "$mode,$bytes,$run,$(tail -1 "$work/m.summary.csv")" >> "$raw"
            if [ "$run" -eq 1 ] && [ "$size" = "$largest" ]; then
                cp "$work/m.occupancy.csv" "$OUT/occupancy/memory_$mode.csv"
            fi
            printf 'memory %s %s run %d: peak RSS %s KiB\n' "$mode" "$size" "$run" \
                "$(tail -1 "$work/m.summary.csv" | cut -d, -f9)"
        done
    done
done

# Median peak RSS and throughput per (mode, size), in first-appearance order.
awk -F, -v OFS=, '
    function median(key, col,    n, i, j, t, a) {
        n = cnt[key]
        for (i = 1; i <= n; i++) a[i] = val[key, col, i] + 0
        for (i = 2; i <= n; i++)
            for (j = i; j > 1 && a[j - 1] > a[j]; j--) { t = a[j]; a[j] = a[j - 1]; a[j - 1] = t }
        return n % 2 ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
    }
    NR == 1 {
        for (c = 4; c <= NF; c++) col[$c] = c
        print "mode,size,runs,peak_rss_kb,throughput_mb_s"
        next
    }
    {
        key = $1 OFS $2
        if (!(key in cnt)) order[++nkeys] = key
        n = ++cnt[key]
        val[key, "rss", n] = $col["peak_rss_kb"]
        val[key, "tput", n] = $col["throughput_mb_s"]
    }
    END {
        for (k = 1; k <= nkeys; k++)
            print order[k], cnt[order[k]], median(order[k], "rss"), median(order[k], "tput")
    }
' "$raw" > "$OUT/memory.csv"

echo "memory ok: medians in $OUT/memory.csv"
