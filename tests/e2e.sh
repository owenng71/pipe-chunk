#!/usr/bin/env bash
# e2e.sh [BINARY] — end-to-end runs of the real binary; every copy is checked with verify.sh.
# BINARY defaults to build/pipe-chunk. Exit 0 iff every case passes.
set -euo pipefail
cd "$(dirname "$0")/.."
bin=${1:-build/pipe-chunk}
[ -x "$bin" ] || { echo "e2e: $bin not found, run make first" >&2; exit 2; }

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0

# run NAME SIZE ARGS...  — copy a fresh SIZE-byte file with ARGS, then verify it
run() {
    local name=$1 size=$2; shift 2
    ./scripts/gen_input.sh "$size" "$tmp/in.bin"
    "$bin" "$@" "$tmp/in.bin" "$tmp/out.bin" || { echo "e2e FAIL: $name: exit $?" >&2; exit 1; }
    ./scripts/verify.sh "$tmp/in.bin" "$tmp/out.bin" >/dev/null || { echo "e2e FAIL: $name" >&2; exit 1; }
    echo "e2e ok: $name"
    pass=$((pass + 1))
}

run "blocking, 100 MiB + 1 byte"        104857601 --mode=blocking
run "blocking, empty input"             0         --mode=blocking
run "blocking, 4 KiB blocks, uneven"    1000001   --mode=blocking -b 4K
run "blocking, 1 MiB blocks"            10M       --mode=blocking -b 1M

./scripts/gen_input.sh 5000001 "$tmp/in.bin"
cat "$tmp/in.bin" | "$bin" --mode=blocking - "$tmp/out.bin"
./scripts/verify.sh "$tmp/in.bin" "$tmp/out.bin" >/dev/null || { echo "e2e FAIL: blocking, stdin pipe" >&2; exit 1; }
echo "e2e ok: blocking, stdin pipe"
pass=$((pass + 1))

echo "e2e: all $pass cases passed"
