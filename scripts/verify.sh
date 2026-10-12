#!/usr/bin/env bash
# verify.sh INPUT OUTPUT [MANIFEST] — exit 0 iff the copy is correct:
#   1. OUTPUT is byte-identical to INPUT
#   2. manifest has every seq 0..N-1 exactly once, blocks tile the file with no gap/overlap
#   3. spot-check: first, last and one random block's sha256 match the manifest
# Manifest format: one "seq,offset,len,sha256hex" line per block, no header, any order.
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: $0 INPUT OUTPUT [MANIFEST]" >&2; exit 2; }
in=$1; out=$2; man=${3:-$2.manifest}
fail() { echo "VERIFY FAIL: $*" >&2; exit 1; }

[ -f "$man" ] || fail "manifest $man missing"
cmp -s "$in" "$out" || fail "output differs from input ($(cmp "$in" "$out" 2>&1 | head -1))"

size=$(stat -c %s "$in")
sorted=$(mktemp); trap 'rm -f "$sorted"' EXIT
sort -t, -k1,1n "$man" > "$sorted"

awk -F, -v size="$size" '
    NF != 4 || $4 !~ /^[0-9a-f]{64}$/ { print "bad line " NR ": " $0; exit 1 }
    $1 != NR - 1   { print "seq " NR - 1 " expected, got " $1 " (lost or duplicated block)"; exit 1 }
    $2 != pos      { print "seq " $1 ": offset " $2 ", expected " pos " (gap or overlap)"; exit 1 }
    $3 <= 0        { print "seq " $1 ": empty block"; exit 1 }
    { pos += $3 }
    END { if (pos != size) { print "blocks cover " pos " bytes, file has " size; exit 1 } }
' "$sorted" >&2 || fail "manifest check"

n=$(wc -l < "$sorted")
if [ "$n" -gt 0 ]; then
    # limit: spot-checks 3 block hashes, not all — full per-block check is too slow in bash for 1 GB files
    for line in 1 "$n" $(( RANDOM % n + 1 )); do
        IFS=, read -r seq off len sha < <(sed -n "${line}p" "$sorted")
        got=$(dd if="$in" bs=1M iflag=skip_bytes,count_bytes skip="$off" count="$len" status=none | sha256sum | cut -d' ' -f1)
        [ "$got" = "$sha" ] || fail "seq $seq: manifest sha256 $sha, actual $got"
    done
fi

echo "VERIFY OK: $n blocks, $size bytes"
