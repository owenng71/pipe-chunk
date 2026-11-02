#!/usr/bin/env bash
# io.sh [BINARY] — I/O strategy comparison. BINARY defaults to build/pipe-chunk.
# Every run is checked with verify.sh; a failed run stops the script. No root needed.
#
# 1. Input strategy: --io=rw vs --io=mmap, page cache warm vs cold, blocking and pipeline modes.
#    The input lives on a real disk (DISK) so it can be dropped from the page cache; the output goes
#    to WORK (RAM) so the writer does not hide the input side. Cold: `dd iflag=nocache count=0`
#    asks the kernel to drop the file's pages (posix_fadvise DONTNEED, allowed for any reader);
#    warm: the file is read once. fincore confirms the state before every run.
# 2. Pipe input: stdin pipe nonblocking (default: O_NONBLOCK + poll) vs --stdin-blocking, with a
#    fast producer (cat from the page cache) and a slow one (PIPE_RATE MB/s). One extra run per
#    config under strace counts read calls, EAGAIN results and poll calls (not timed).
#
# Settings come from the environment (defaults in brackets):
#   SIZE       input size for experiment 1 [1G]
#   PIPE_SIZE  input size for experiment 2 [512M]
#   PIPE_RATE  slow producer speed in MB/s [200]
#   RUNS       runs per configuration [5]
#   DISK       directory on a real disk for the input [bench/results]
#   OUT        results directory [bench/results]
#   WORK       where outputs go [/dev/shm if it exists, else OUT]
# Pipeline runs use -q 16 -w 4 -b 64K, the blocking runs -b 64K.
#
# Output in OUT:
#   io_input_raw.csv, io_input.csv  mode,io,cache + metrics (per run / medians)
#   io_pipe_raw.csv, io_pipe.csv    producer,stdin + metrics; io_pipe.csv adds read_calls,
#                                   eagain, poll_calls from the strace run
set -euo pipefail
# awk parses "0.017" as 0 under a locale with a decimal comma (e.g. LC_NUMERIC=vi_VN).
export LC_ALL=C
cd "$(dirname "$0")/.."
bin=${1:-build/pipe-chunk}
[ -x "$bin" ] || { echo "io: $bin not found, run make first" >&2; exit 2; }

SIZE=${SIZE:-1G}
PIPE_SIZE=${PIPE_SIZE:-512M}
PIPE_RATE=${PIPE_RATE:-200}
RUNS=${RUNS:-5}
DISK=${DISK:-bench/results}
OUT=${OUT:-bench/results}
if [ -z "${WORK:-}" ]; then
    if [ -d /dev/shm ] && [ -w /dev/shm ]; then WORK=/dev/shm; else WORK=$OUT; fi
fi

fail() { echo "io FAIL: $*" >&2; exit 1; }
for tool in fincore strace python3; do
    command -v "$tool" >/dev/null || fail "$tool not found"
done

mkdir -p "$OUT" "$DISK"
[ "$(df --output=fstype "$DISK" | tail -1)" != tmpfs ] || fail "DISK=$DISK is tmpfs: cold-cache runs need a real disk"
disk=$(mktemp -d "$DISK/.pipe-chunk-io.XXXXXX")
work=$(mktemp -d "$WORK/pipe-chunk-io.XXXXXX")
trap 'rm -rf "$disk" "$work"' EXIT
trap 'exit 130' INT TERM

resident() { fincore -b -n -o RES "$1" | tr -d ' '; }

# set_cache FILE warm|cold — and check that the kernel really did it
set_cache() {
    local size res
    size=$(stat -c %s "$1")
    if [ "$2" = cold ]; then
        sync "$1"   # dirty pages (a freshly written file) cannot be dropped
        dd if="$1" iflag=nocache count=0 status=none
        res=$(resident "$1")
        [ "$res" -le $((size / 100)) ] || fail "could not drop $1 from the page cache ($res bytes resident)"
    else
        cat "$1" > /dev/null
        res=$(resident "$1")
        [ "$res" -ge $((size / 100 * 99)) ] || fail "could not cache $1 ($res of $size bytes resident)"
    fi
}

# slow_cat FILE MB/s — write FILE to stdout in 64 KiB chunks at a steady rate
slow_cat() {
    python3 -c '
import os, sys, time
path, rate = sys.argv[1], float(sys.argv[2]) * 1e6
sent, t0 = 0, time.monotonic()
with open(path, "rb", buffering=0) as f:
    while True:
        chunk = f.read(65536)
        if not chunk:
            break
        view = memoryview(chunk)
        while view:
            view = view[os.write(1, view):]
        sent += len(chunk)
        wait = t0 + sent / rate - time.monotonic()
        if wait > 0:
            time.sleep(wait)
' "$1" "$2"
}

# producer fast|slow FILE
producer() {
    if [ "$1" = fast ]; then cat "$2"; else slow_cat "$2" "$PIPE_RATE"; fi
}

# check_run NAME RC — stop on Ctrl+C, a failed copy or a wrong output
check_run() {
    [ "$2" -ne 130 ] || { echo "io: interrupted" >&2; exit 130; }
    [ "$2" -eq 0 ] || fail "$1: exit $2 ($(cat "$work/err"))"
    ./scripts/verify.sh "$3" "$work/out.bin" >/dev/null || fail "$1"
}

pipeline_args=(-q 16 -w 4 -b 64K)
blocking_args=(--mode=blocking -b 64K)

# --- 1. input strategy -----------------------------------------------------------------------
./scripts/gen_input.sh "$SIZE" "$disk/in.bin"
raw=$OUT/io_input_raw.csv
header=
for run in $(seq 1 "$RUNS"); do
    for mode in blocking pipeline; do
        for io in rw mmap; do
            for cache in warm cold; do
                name="$mode $io $cache run $run"
                if [ "$mode" = blocking ]; then args=("${blocking_args[@]}"); else args=("${pipeline_args[@]}"); fi
                rm -f "$work/out.bin" "$work/out.bin.manifest"
                set_cache "$disk/in.bin" "$cache"
                rc=0
                "$bin" "${args[@]}" --io="$io" --metrics="$work/m" \
                    "$disk/in.bin" "$work/out.bin" 2>"$work/err" || rc=$?
                check_run "$name" "$rc" "$disk/in.bin"
                if [ -z "$header" ]; then
                    header=$(head -1 "$work/m.summary.csv")
                    echo "mode,io,cache,run,$header" > "$raw"
                fi
                echo "$mode,$io,$cache,$run,$(tail -1 "$work/m.summary.csv")" >> "$raw"
                printf 'io %s: %s MB/s\n' "$name" "$(tail -1 "$work/m.summary.csv" | cut -d, -f4)"
            done
        done
    done
done
awk -v keys=3 -f bench/median.awk "$raw" > "$OUT/io_input.csv"
rm -f "$disk/in.bin"

# --- 2. pipe input ---------------------------------------------------------------------------
./scripts/gen_input.sh "$PIPE_SIZE" "$work/pipe.bin"
raw=$OUT/io_pipe_raw.csv
header=
for run in $(seq 1 "$RUNS"); do
    for prod in fast slow; do
        for stdin in nonblocking blocking; do
            name="pipe $prod producer, $stdin stdin, run $run"
            args=("${pipeline_args[@]}")
            [ "$stdin" = nonblocking ] || args+=(--stdin-blocking)
            rm -f "$work/out.bin" "$work/out.bin.manifest"
            rc=0
            producer "$prod" "$work/pipe.bin" | "$bin" "${args[@]}" --metrics="$work/m" - "$work/out.bin" \
                2>"$work/err" || rc=$?
            check_run "$name" "$rc" "$work/pipe.bin"
            if [ -z "$header" ]; then
                header=$(head -1 "$work/m.summary.csv")
                echo "producer,stdin,run,$header" > "$raw"
            fi
            echo "$prod,$stdin,$run,$(tail -1 "$work/m.summary.csv")" >> "$raw"
            printf 'io %s: %s MB/s\n' "$name" "$(tail -1 "$work/m.summary.csv" | cut -d, -f4)"
        done
    done
done
awk -v keys=2 -f bench/median.awk "$raw" > "$work/pipe_medians.csv"

# System-call counts: one untimed strace run per config (strace slows the copy down).
echo "producer,stdin,read_calls,eagain,poll_calls" > "$work/syscalls.csv"
for prod in fast slow; do
    for stdin in nonblocking blocking; do
        args=("${pipeline_args[@]}")
        [ "$stdin" = nonblocking ] || args+=(--stdin-blocking)
        rm -f "$work/out.bin" "$work/out.bin.manifest"
        rc=0
        producer "$prod" "$work/pipe.bin" | strace -f -c -o "$work/strace.txt" -e trace=read,poll,ppoll \
            "$bin" "${args[@]}" - "$work/out.bin" 2>"$work/err" || rc=$?
        check_run "strace $prod $stdin" "$rc" "$work/pipe.bin"
        # strace -c columns: % time, seconds, usecs/call, calls, [errors], syscall
        awk -v OFS=, -v p="$prod" -v s="$stdin" '
            $NF == "read"                  { reads = $4; eagain = NF == 6 ? $5 : 0 }
            $NF == "poll" || $NF == "ppoll" { polls += $4 }
            END { print p, s, reads + 0, eagain + 0, polls + 0 }
        ' "$work/strace.txt" >> "$work/syscalls.csv"
        echo "io strace $prod producer, $stdin stdin: $(tail -1 "$work/syscalls.csv")"
    done
done
awk -F, -v OFS=, '
    NR == FNR { sys[$1 OFS $2] = $3 OFS $4 OFS $5; next }
    { print $0, sys[$1 OFS $2] }
' "$work/syscalls.csv" "$work/pipe_medians.csv" > "$OUT/io_pipe.csv"

echo "io ok: medians in $OUT/io_input.csv and $OUT/io_pipe.csv"
