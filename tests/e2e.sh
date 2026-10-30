#!/usr/bin/env bash
# e2e.sh [BINARY] — end-to-end runs of the real binary; every copy is checked with verify.sh.
# BINARY defaults to build/pipe-chunk. Exit 0 iff every case passes.
set -euo pipefail
cd "$(dirname "$0")/.."
bin=${1:-build/pipe-chunk}
[ -x "$bin" ] || { echo "e2e: $bin not found, run make first" >&2; exit 2; }

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0

fail() { echo "e2e FAIL: $*" >&2; exit 1; }

# run NAME SIZE ARGS...  — copy a fresh SIZE-byte file with ARGS, then verify it
run() {
    local name=$1 size=$2; shift 2
    ./scripts/gen_input.sh "$size" "$tmp/in.bin"
    "$bin" "$@" "$tmp/in.bin" "$tmp/out.bin" 2>"$tmp/err" || fail "$name: exit $? ($(cat "$tmp/err"))"
    ./scripts/verify.sh "$tmp/in.bin" "$tmp/out.bin" >/dev/null || fail "$name"
    echo "e2e ok: $name"
    pass=$((pass + 1))
}

# run_stdin NAME SIZE ARGS...  — same, with the input arriving through a pipe on stdin
run_stdin() {
    local name=$1 size=$2; shift 2
    ./scripts/gen_input.sh "$size" "$tmp/in.bin"
    cat "$tmp/in.bin" | "$bin" "$@" - "$tmp/out.bin" 2>"$tmp/err" || fail "$name: exit $? ($(cat "$tmp/err"))"
    ./scripts/verify.sh "$tmp/in.bin" "$tmp/out.bin" >/dev/null || fail "$name"
    echo "e2e ok: $name"
    pass=$((pass + 1))
}

# peak_rss_kb ARGS...  — copy $tmp/in.bin with ARGS and --metrics, verify, print peak RSS in KiB
peak_rss_kb() {
    "$bin" "$@" --metrics="$tmp/m" "$tmp/in.bin" "$tmp/out.bin" 2>"$tmp/err" || fail "memory run $*: $(cat "$tmp/err")"
    ./scripts/verify.sh "$tmp/in.bin" "$tmp/out.bin" >/dev/null || fail "memory run $*"
    awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) if ($i == "peak_rss_kb") c = i } NR == 2 { print $c }' "$tmp/m.summary.csv"
}

run       "blocking, 100 MiB + 1 byte"          104857601 --mode=blocking
run       "blocking, empty input"               0         --mode=blocking
run       "blocking, 4 KiB blocks, uneven"      1000001   --mode=blocking -b 4K
run       "blocking, 1 MiB blocks"              10M       --mode=blocking -b 1M
run_stdin "blocking, stdin pipe"                5000001   --mode=blocking
run       "blocking, mmap input"                10000001  --mode=blocking --io=mmap

run       "pipeline, 100 MiB + 1 byte"          104857601
run       "pipeline, empty input"               0
run       "pipeline, 1 worker, queue 1"         5000001   -w 1 -q 1
run       "pipeline, 8 workers, queue 64, 4 KiB" 10000001 -w 8 -q 64 -b 4K
run       "pipeline, 1 MiB blocks"              10M       -b 1M
run       "pipeline, unbounded"                 10000001  --unbounded
run       "pipeline, slow input"                2000000   --in-rate=20
run_stdin "pipeline, stdin pipe"                5000001   -w 4
run       "pipeline, mmap input"                104857601 --io=mmap
run       "pipeline, mmap, empty input"         0         --io=mmap

# Backpressure: with a slow writer (64 MB at 32 MB/s) the bounded pipeline holds at most
# 2*4 + 4 + 2 = 14 blocks of 64 KiB; --unbounded keeps reading and buffers most of the file.
./scripts/gen_input.sh 64000000 "$tmp/in.bin"
bounded=$(peak_rss_kb -q 4 -w 4 -b 64K --out-rate=32)
unbounded=$(peak_rss_kb -q 4 -w 4 -b 64K --out-rate=32 --unbounded)
echo "e2e: peak RSS with slow output: bounded ${bounded} KiB, unbounded ${unbounded} KiB"
[ $((unbounded - bounded)) -gt 32000 ] || fail "unbounded run did not grow memory (bounded ${bounded}, unbounded ${unbounded} KiB)"
case $bin in
    *build-*) ;;   # sanitizer runtimes add their own memory: only the difference is meaningful
    *) [ "$bounded" -lt 32000 ] || fail "bounded run used ${bounded} KiB, limit 32000" ;;
esac
echo "e2e ok: memory stays bounded with slow output"
pass=$((pass + 1))

echo "e2e: all $pass cases passed"
