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

# run_fails NAME SIZE ARGS...  — the copy must give up with exit 1
run_fails() {
    local name=$1 size=$2 rc=0; shift 2
    ./scripts/gen_input.sh "$size" "$tmp/in.bin"
    "$bin" "$@" "$tmp/in.bin" "$tmp/out.bin" 2>"$tmp/err" || rc=$?
    [ "$rc" -eq 1 ] || fail "$name: exit $rc, expected 1"
    echo "e2e ok: $name"
    pass=$((pass + 1))
}

# listed_blocks_ok  — no seq twice in the manifest, and each listed block of out.bin matches it and in.bin
listed_blocks_ok() {
    local seq off len sha got want
    [ -z "$(cut -d, -f1 "$tmp/out.bin.manifest" | sort | uniq -d)" ] || return 1
    while IFS=, read -r seq off len sha; do
        got=$(dd if="$tmp/out.bin" bs=1M iflag=skip_bytes,count_bytes skip="$off" count="$len" status=none | sha256sum)
        want=$(dd if="$tmp/in.bin" bs=1M iflag=skip_bytes,count_bytes skip="$off" count="$len" status=none | sha256sum)
        [ "${got%% *}" = "$sha" ] && [ "${want%% *}" = "$sha" ] || return 1
    done < "$tmp/out.bin.manifest"
}

# run_sigint NAME SIGNALS ARGS...  — copy a 20 MB file throttled by ARGS, send SIGINT after 0.5 s
# (and again 0.1 s later if SIGNALS is 2), then check exit 130 and that the output holds only correct
# blocks. One signal drains: the manifest is blocks 0..k-1 (0 < k < all) and out.bin is exactly
# their bytes. Two signals stop at once: any set of complete blocks, no "interrupted" line.
run_sigint() {
    local name=$1 signals=$2 pid rc=0 k bytes; shift 2
    ./scripts/gen_input.sh 20000000 "$tmp/in.bin"
    "$bin" "$@" "$tmp/in.bin" "$tmp/out.bin" 2>"$tmp/err" & pid=$!
    sleep 0.5
    kill -INT "$pid"
    if [ "$signals" -eq 2 ]; then sleep 0.1; kill -INT "$pid"; fi
    wait "$pid" || rc=$?
    [ "$rc" -eq 130 ] || fail "$name: exit $rc, expected 130 ($(cat "$tmp/err"))"
    k=$(wc -l < "$tmp/out.bin.manifest")
    [ "$k" -gt 0 ] && [ "$k" -lt $((20000000 / 65536 + 1)) ] || fail "$name: $k blocks listed, expected a part of the file"
    listed_blocks_ok || fail "$name: manifest lists a duplicate or wrong block"
    if [ "$signals" -eq 1 ]; then
        grep -q "interrupted: $k blocks" "$tmp/err" || fail "$name: no drain message ($(cat "$tmp/err"))"
        bytes=$(awk -F, '{ n += $3 } END { print n }' "$tmp/out.bin.manifest")
        head -c "$bytes" "$tmp/in.bin" > "$tmp/prefix.bin"
        ./scripts/verify.sh "$tmp/prefix.bin" "$tmp/out.bin" "$tmp/out.bin.manifest" >/dev/null ||
            fail "$name: output is not the first $k blocks"
    else
        ! grep -q "interrupted:" "$tmp/err" || fail "$name: second SIGINT drained instead of stopping"
    fi
    echo "e2e ok: $name ($k blocks)"
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
run_stdin "pipeline, blocking stdin pipe"       5000001   -w 4 --stdin-blocking
run_stdin "blocking, blocking stdin pipe"       5000001   --mode=blocking --stdin-blocking
run       "pipeline, mmap input"                104857601 --io=mmap
run       "pipeline, mmap, empty input"         0         --io=mmap

# Fault matrix (W2-10). Fixed seed: every thread's fault stream is reproducible.
faults="--fault-short=0.3 --fault-eintr=0.3 --fault-eagain=0.3 --seed=7"
run       "pipeline, short I/O"                 10000001  --fault-short=0.3 --seed=7
run       "pipeline, EINTR"                     10000001  --fault-eintr=0.3 --seed=7
run       "pipeline, EAGAIN"                    10000001  --fault-eagain=0.3 --seed=7
run       "pipeline, I/O delays"                2000001   --fault-delay-us=200 --seed=7
run       "pipeline, all faults, slow output"   5000001   $faults --out-rate=20
run       "pipeline, all faults, mmap"          10000001  $faults --io=mmap
run_stdin "pipeline, all faults, stdin pipe"    5000001   $faults
run       "blocking, all faults"                10000001  $faults --mode=blocking
run_stdin "blocking, all faults, stdin pipe"    5000001   $faults --mode=blocking
# One worker makes the deaths reproducible; with P = 1 every retry dies and the run must fail.
run       "pipeline, dying worker"              2000001   -w 1 --fail-worker=0.2 --seed=7
run       "pipeline, dying worker, mmap, faults" 2000001  -w 1 --fail-worker=0.2 --io=mmap $faults
run_fails "pipeline, worker always dies"        1000001   --fail-worker=1

# Ctrl+C mid-run: the input is throttled so the signal lands mid-copy.
run_sigint "pipeline, SIGINT drains"            1 --in-rate=10
run_sigint "pipeline, mmap, SIGINT drains"      1 --in-rate=10 --io=mmap
run_sigint "blocking, SIGINT drains"            1 --in-rate=10 --mode=blocking
run_sigint "pipeline, second SIGINT stops"      2 -q 64 --out-rate=5

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
