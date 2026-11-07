#!/usr/bin/env bash
# inflight.sh [BINARY] — does throughput depend on the bytes in flight rather than the block size?
# Runs bench/sweep.sh with 4 workers for 64K, 256K and 1M blocks, with queue sizes chosen so that
# the bytes the two queues can hold (2 x queue x block) are 2, 8, 32 and 128 MiB for every block
# size. If throughput falls at the same byte count for all three, the cause is the working set
# outgrowing the CPU cache, not the block size itself.
#
# Settings: SIZE and RUNS are passed through to sweep.sh; OUT is the results directory
# [bench/results]. Results: OUT/inflight/{64K,256K,1M}/sweep.csv (+ the other sweep.sh files).
set -euo pipefail
cd "$(dirname "$0")/.."
bin=${1:-build/pipe-chunk}
OUT=${OUT:-bench/results}

run() {   # run BLOCK "QUEUES"
    WORKERS=4 BLOCKS=$1 QUEUES=$2 OUT=$OUT/inflight/$1 bench/sweep.sh "$bin" | tail -1
}
run 64K  "16 64 256 1024"
run 256K "4 16 64 256"
run 1M   "1 4 16 64"
