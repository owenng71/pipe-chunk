#!/usr/bin/env bash
# gen_input.sh SIZE FILE — random test file. SIZE accepts head -c suffixes: 100M, 1G, 12345.
# Use an odd byte count (e.g. 1000001) to exercise a short last block.
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 SIZE FILE" >&2; exit 2; }
head -c "$1" /dev/urandom > "$2"
