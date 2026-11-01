#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
SWEEP="$ROOT/bench/sweep.sh"
RESULTS_ROOT="$ROOT/bench/results"
TEST_ROOT=$(mktemp -d "$RESULTS_ROOT/.test-sweep.XXXXXX")
INPUT_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/w3-01-test-input.XXXXXX")
STUB_BIN="$TEST_ROOT/pipe-chunk-stub"
STUB_LOG="$TEST_ROOT/invocations.tsv"
SYNC_LOG="$TEST_ROOT/sync.log"
SUDO_LOG="$TEST_ROOT/sudo.log"
VERIFY_LOG="$TEST_ROOT/verify.log"
INPUT="$INPUT_ROOT/input.bin"
LARGE_INPUT="$INPUT_ROOT/input-1g.bin"
FIXTURE_DIR="$TEST_ROOT/fixtures"
symlink_escape=
mkdir -p "$FIXTURE_DIR" "$TEST_ROOT/bin"
cleanup() {
  [[ -z "$symlink_escape" ]] || rm -f -- "$symlink_escape"
  rm -rf -- "$TEST_ROOT" "$INPUT_ROOT"
}
trap cleanup EXIT

fail() { printf 'test_sweep: %s\n' "$*" >&2; exit 1; }
assert_eq() { [[ "$1" == "$2" ]] || fail "$3 (expected '$2', got '$1')"; }
assert_contains() { [[ "$1" == *"$2"* ]] || fail "$3 (missing '$2')"; }
assert_file() { [[ -f "$1" ]] || fail "missing file: $1"; }

cat >"$STUB_BIN" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
prefix= mode= queue= workers= block= rate= manifest= unbounded=0
for arg in "$@"; do
  case "$arg" in
    --metrics=*) prefix=${arg#--metrics=} ;;
    --mode=*) mode=${arg#--mode=} ;;
    --queue=*) queue=${arg#--queue=} ;;
    --workers=*) workers=${arg#--workers=} ;;
    --block-size=*) block=${arg#--block-size=} ;;
    --out-rate=*) rate=${arg#--out-rate=} ;;
    --manifest=*) manifest=${arg#--manifest=} ;;
    --unbounded) unbounded=1 ;;
  esac
done
[[ -n "$prefix" ]] || { echo 'missing metrics prefix' >&2; exit 90; }
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$prefix" "$mode" "$queue" "$workers" "$block" "$rate" "$unbounded" "$manifest" >>"$STUB_LOG"
printf 'STUB_RUN %s\n' "$prefix"
if [[ -n ${STUB_FAIL_PREFIX:-} && "$prefix" == *"$STUB_FAIL_PREFIX"* ]]; then exit 17; fi
trial=${prefix##*_t}
trial=${trial%%.*}
case "$trial" in ''|*[!0-9]*) trial=1 ;; esac
values=(9 10 100 2 30)
value=${values[$(((trial - 1) % 5))]}
mkdir -p "$(dirname -- "$prefix")"
cat >"$prefix.summary.csv" <<'CSV'
bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s
100000,20,1.0,VALUE,VALUE,VALUE,0.1,0.2,VALUE,0.01
CSV
sed -i "s/VALUE/$value/g" "$prefix.summary.csv"
cat >"$prefix.occupancy.csv" <<'CSV'
t_ms,q1_size,q2_size
0,0,0
10,1,2
CSV
STUB
chmod +x "$STUB_BIN"
truncate -s 1073741824 "$INPUT"
truncate -s 1073741824 "$LARGE_INPUT"

cat >"$TEST_ROOT/bin/sync" <<'SYNC'
#!/usr/bin/env bash
printf 'sync\n' >>"$SYNC_LOG"
[[ ${SYNC_FAIL:-0} != 1 ]]
SYNC
cat >"$TEST_ROOT/bin/sudo" <<'SUDO'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"$SUDO_LOG"
[[ ${SUDO_DENY:-0} != 1 ]] || exit 1
if [[ ${1:-} == -n && ${2:-} == true ]]; then exit 0; fi
if [[ ${1:-} == -n && ${2:-} == tee ]]; then
  [[ ${SUDO_DROP_FAIL:-0} != 1 ]] || exit 1
  cat >"${3:?missing drop target}"
  exit 0
fi
exit 2
SUDO
cat >"$TEST_ROOT/verify-ok" <<'VERIFY'
#!/usr/bin/env bash
printf '%s\t%s\t%s\n' "${1:-}" "${2:-}" "${3:-}" >>"$VERIFY_LOG"
printf 'VERIFY OK\n'
VERIFY
cat >"$TEST_ROOT/verify-fail" <<'VERIFY'
#!/usr/bin/env bash
printf '%s\t%s\t%s\n' "${1:-}" "${2:-}" "${3:-}" >>"$VERIFY_LOG"
printf 'VERIFY FAIL\n' >&2
exit 1
VERIFY
chmod +x "$TEST_ROOT/bin/sync" "$TEST_ROOT/bin/sudo" "$TEST_ROOT/verify-ok" "$TEST_ROOT/verify-fail"
printf 'Linux test kernel\n' >"$FIXTURE_DIR/proc-linux"
printf 'Linux version Microsoft WSL\n' >"$FIXTURE_DIR/proc-wsl"
printf '3\n' >"$FIXTURE_DIR/drop-caches"

export PIPE_CHUNK_TESTING=1 PIPE_CHUNK_BIN="$STUB_BIN" STUB_LOG SYNC_LOG SUDO_LOG VERIFY_LOG
export RUNS SWEEP_TEST_UID WSL_DISTRO_NAME PIPE_CHUNK_PROC_VERSION_FILE PIPE_CHUNK_DROP_CACHES_FILE PIPE_CHUNK_VERIFY_SCRIPT
export STUB_FAIL_PREFIX SUDO_DENY SUDO_DROP_FAIL SYNC_FAIL
export PATH="$TEST_ROOT/bin:$PATH"
export INPUT_SIZE=8388608
unset RUNS SWEEP_TEST_UID WSL_DISTRO_NAME || true

new_out() { printf '%s/%s' "$TEST_ROOT" "$1"; }
run_sweep() {
  local out=$1; shift
  bash "$SWEEP" --bin "$PIPE_CHUNK_BIN" --input "$INPUT" --cache=warm --out-dir "$out" "$@" >"${SWEEP_CAPTURE:-/dev/null}"
}
expect_failure() {
  local out=$1; shift
  if run_sweep "$out" "$@" >"$TEST_ROOT/failure.out" 2>"$TEST_ROOT/failure.err"; then
    fail "expected sweep failure for $out"
  fi
}
count_lines() { awk 'END { print NR + 0 }' "$1"; }
assert_log_count() { assert_eq "$(count_lines "$STUB_LOG")" "$1" "$2"; }
assert_header() {
  local file=$1 expected=$2
  assert_file "$file"
  assert_eq "$(head -n 1 "$file")" "$expected" "header $file"
}
assert_median() {
  local file=$1 group=$2 mode=$3 queue=$4 workers=$5 block=$6 metric=$7 expected=$8
  awk -F, -v g="$group" -v m="$mode" -v q="$queue" -v w="$workers" -v b="$block" -v metric="$metric" -v expected="$expected" '
    NR==1 { for(i=1;i<=NF;i++) col[$i]=i; next }
    $(col["run_group"])==g && $(col["mode"])==m && $(col["queue_cap"])==q && $(col["workers"])==w && $(col["block_bytes"])==b {
      if ($(col[metric]) != expected) exit 1
      found++
    }
    END { if(found!=1) exit 1 }
  ' "$file" || fail "median $metric for $group/$mode q=$queue w=$workers b=$block is not $expected"
}
assert_no_median() {
  local file=$1 group=$2 mode=$3 queue=$4 workers=$5 block=$6
  awk -F, -v g="$group" -v m="$mode" -v q="$queue" -v w="$workers" -v b="$block" '
    NR==1 { for(i=1;i<=NF;i++) col[$i]=i; next }
    $(col["run_group"])==g && $(col["mode"])==m && $(col["queue_cap"])==q && $(col["workers"])==w && $(col["block_bytes"])==b { found++ }
    END { exit(found==0 ? 0 : 1) }
  ' "$file" || fail "incomplete group unexpectedly has median: $group/$mode q=$queue w=$workers b=$block"
}
assert_raw_metadata() {
  local file=$1
  awk -F, 'NR==1 { for(i=1;i<=NF;i++) col[$i]=i; next }
    $(col["input_bytes"])=="" || $(col["cache_method"])=="" { exit 1 }
    END { if(NR<2) exit 1 }
  ' "$file" || fail "raw rows missing input_bytes/cache_method: $file"
}
assert_unique_prefixes() {
  local expected=$1 log=$2
  local total unique
  total=$(count_lines "$log")
  unique=$(cut -f1 "$log" | sort -u | wc -l | tr -d ' ')
  assert_eq "$total" "$expected" 'stub invocation count'
  assert_eq "$unique" "$expected" 'metrics prefixes must be unique'
}

TRIALS_HEADER='bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s,run_group,mode,status,queue_cap,workers,block_bytes,input_bytes,cache_method,trial,out_rate_mb_s'
MEDIANS_HEADER='bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s,run_group,mode,queue_cap,workers,block_bytes,input_bytes,cache_method,out_rate_mb_s'

# Default RUNS: core + baseline + memory counts, tuple multiplicity, schema, and numeric medians.
OUT=$(new_out default-five)
: >"$STUB_LOG"
run_sweep "$OUT" --out-rate=0.1
assert_log_count 265 'default five-trial total'
assert_unique_prefixes 265 "$STUB_LOG"
assert_eq "$(awk -F '\t' '$1 ~ /metrics_core_/ {n++} END{print n+0}' "$STUB_LOG")" 240 'core invocation count'
assert_eq "$(awk -F '\t' '$2=="blocking" {n++} END{print n+0}' "$STUB_LOG")" 15 'baseline invocation count'
assert_eq "$(awk -F '\t' '$1 ~ /metrics_memory_/ {n++} END{print n+0}' "$STUB_LOG")" 10 'memory invocation count'
awk -F '\t' '$1 ~ /metrics_core_/ { key=$3 ":" $4 ":" $5; count[key]++ } END { for(k in count) if(count[k]!=5) exit 1; if(length(count)!=48) exit 1 }' "$STUB_LOG" || fail 'core tuple trial counts'
assert_header "$OUT/trials.csv" "$TRIALS_HEADER"
assert_header "$OUT/medians.csv" "$MEDIANS_HEADER"
assert_header "$OUT/memory_trials.csv" "$TRIALS_HEADER"
assert_header "$OUT/memory_medians.csv" "$MEDIANS_HEADER"
assert_raw_metadata "$OUT/trials.csv"
assert_raw_metadata "$OUT/memory_trials.csv"
assert_median "$OUT/medians.csv" core pipeline 16 1 4096 throughput_mb_s 10
assert_median "$OUT/medians.csv" core pipeline 16 1 4096 lat_p50_us 10
assert_median "$OUT/medians.csv" core pipeline 16 1 4096 peak_rss_kb 10
assert_median "$OUT/medians.csv" baseline blocking '' 1 4096 throughput_mb_s 10
assert_median "$OUT/medians.csv" baseline blocking '' 1 4096 lat_p50_us 10
assert_median "$OUT/medians.csv" baseline blocking '' 1 4096 peak_rss_kb 10
assert_median "$OUT/memory_medians.csv" memory bounded 4 4 65536 throughput_mb_s 10
assert_median "$OUT/memory_medians.csv" memory bounded 4 4 65536 lat_p50_us 10
assert_median "$OUT/memory_medians.csv" memory bounded 4 4 65536 peak_rss_kb 10
assert_eq "$(find "$OUT" -maxdepth 1 -name 'occ_q*_w*_b*_t1.csv' | wc -l | tr -d ' ')" 48 'retained trial-one core occupancy count'
assert_eq "$(find "$OUT" -maxdepth 1 -name 'metrics_memory_*.occupancy.csv' | wc -l | tr -d ' ')" 10 'retained memory occupancy count'
git -C "$ROOT" check-ignore -q "$OUT/trials.csv" || fail 'bench/results output is not ignored'
awk -F '\t' '$1 ~ /metrics_memory_(bounded|unbounded)_/ { match($1, /_t[0-9]+$/); trial=substr($1, RSTART); key=$3 ":" $4 ":" $5 ":" $6 ":" trial; u=($1 ~ /_unbounded_/); if(key in flags && flags[key]==u) exit 1; flags[key]=u; count[key]++ } END { for(k in count) if(count[k]!=2) exit 1; if(length(count)!=5) exit 1 }' "$STUB_LOG" || fail 'bounded/unbounded settings differ beyond --unbounded'

# RUNS=1 matrix and memory default/override recording.
OUT=$(new_out one-run)
: >"$STUB_LOG"
RUNS=1 INPUT_SIZE=67108864 run_sweep "$OUT"
assert_log_count 53 'RUNS=1 total'
assert_eq "$(awk -F '\t' '$1 ~ /metrics_core_/ {n++} END{print n+0}' "$STUB_LOG")" 48 'RUNS=1 core count'
assert_eq "$(awk -F '\t' '$2=="blocking" {n++} END{print n+0}' "$STUB_LOG")" 3 'RUNS=1 baseline count'
assert_eq "$(awk -F '\t' '$1 ~ /metrics_memory_/ {n++} END{print n+0}' "$STUB_LOG")" 2 'RUNS=1 memory count'
assert_contains "$(cat "$OUT/memory_trials.csv")" ',4,4,65536,67108864,warm,1,10' 'memory defaults and metadata'

OUT=$(new_out memory-overrides)
: >"$STUB_LOG"
RUNS=1 INPUT_SIZE=67108864 run_sweep "$OUT" --memory-queue=7 --memory-workers=2 --memory-block-size=4096 --out-rate=0.01
grep -q ',7,2,4096,67108864,warm,1,0.01$' "$OUT/memory_trials.csv" || { cat "$OUT/memory_trials.csv" >&2; fail 'memory overrides not recorded'; }
awk -F '\t' '$1 ~ /metrics_memory_/ {if($3!=7 || $4!=2 || $5!=4096 || $6!=0.01) exit 1; n++} END{if(n!=2) exit 1}' "$STUB_LOG" || fail 'memory override flags not passed'

# A failed trial remains visible, incomplete group gets no median, later runs continue and exit is nonzero.
OUT=$(new_out failed-trial)
: >"$STUB_LOG"
if RUNS=5 STUB_FAIL_PREFIX=core_q1_w1_b4096_t01 run_sweep "$OUT" --out-rate=0.1 >"$TEST_ROOT/failed.out" 2>"$TEST_ROOT/failed.err"; then fail 'injected failed trial returned success'; fi
assert_log_count 265 'ordinary failure should finish sweep'
assert_raw_metadata "$OUT/trials.csv"
awk -F, 'NR==1 {for(i=1;i<=NF;i++) col[$i]=i; next} $(col["status"])=="failed" {failed++; for(i=1;i<=10;i++) if($i!="") exit 1} END{if(failed!=1) exit 1}' "$OUT/trials.csv" || fail 'failed row status/metrics'
assert_no_median "$OUT/medians.csv" core pipeline 1 1 4096

# Default estimate (sparse 1 GiB source) and smoke estimate occur before the first memory call.
OUT=$(new_out estimate-default)
: >"$STUB_LOG"
env -u INPUT_SIZE -u RUNS bash "$SWEEP" --bin "$PIPE_CHUNK_BIN" --input "$LARGE_INPUT" --cache=warm --out-dir "$OUT" >"$TEST_ROOT/estimate-default.out"
assert_contains "$(cat "$TEST_ROOT/estimate-default.out")" '00:17:54' 'default memory duration estimate'
assert_contains "$(cat "$TEST_ROOT/estimate-default.out")" 'decimal MB/s' 'estimate units'
assert_contains "$(cat "$TEST_ROOT/estimate-default.out")" 'output-bound' 'estimate assumption'
assert_contains "$(cat "$TEST_ROOT/estimate-default.out")" 'setup/teardown' 'estimate overhead note'
awk '/00:17:54/ { estimate=NR } /STUB_RUN .*metrics_memory_/ { memory=NR; exit } END { if(!estimate || !memory || estimate>=memory) exit 1 }' "$TEST_ROOT/estimate-default.out" || fail 'default estimate must precede first memory invocation'
awk 'FNR==1 { if($0 ~ /memory/) memory=NR }' "$TEST_ROOT/estimate-default.out" >/dev/null
OUT=$(new_out estimate-smoke)
: >"$STUB_LOG"
RUNS=1 INPUT_SIZE=67108864 SWEEP_CAPTURE="$TEST_ROOT/estimate-smoke.out" run_sweep "$OUT"
assert_contains "$(cat "$TEST_ROOT/estimate-smoke.out")" '00:00:14' 'smoke memory duration estimate'
assert_contains "$(cat "$TEST_ROOT/estimate-smoke.out")" 'decimal MB/s' 'smoke estimate units'
assert_contains "$(cat "$TEST_ROOT/estimate-smoke.out")" 'output-bound' 'smoke estimate assumption'
assert_contains "$(cat "$TEST_ROOT/estimate-smoke.out")" 'setup/teardown' 'smoke estimate overhead note'
awk '/00:00:14/ { estimate=NR } /STUB_RUN .*metrics_memory_/ { memory=NR; exit } END { if(!estimate || !memory || estimate>=memory) exit 1 }' "$TEST_ROOT/estimate-smoke.out" || fail 'smoke estimate must precede first memory invocation'

# Verifier is called only for trial-one core runs; a failed first verification aborts immediately.
OUT=$(new_out verify-success)
: >"$STUB_LOG"; : >"$VERIFY_LOG"
RUNS=5 PIPE_CHUNK_VERIFY_SCRIPT="$TEST_ROOT/verify-ok" run_sweep "$OUT" --verify --out-rate=0.1
assert_eq "$(count_lines "$VERIFY_LOG")" 48 'verifier call count'
awk -F '\t' '$1!="" && $2 ~ /_t01/ && $3!="" {n++} END{if(n!=48) exit 1}' "$VERIFY_LOG" || fail 'verifier should receive only core trial-one input/output/manifest'
awk -F '\t' '$1 ~ /metrics_core_.*_t01$/ {if($8=="") exit 1; n++} END{if(n!=48) exit 1}' "$STUB_LOG" || fail 'manifest flag must be present on each core trial-one invocation'
awk -F '\t' '$1 !~ /metrics_core_.*_t01$/ && $8!="" {exit 1}' "$STUB_LOG" || fail 'manifest flag must not be passed to other invocations'
OUT=$(new_out verify-failure)
: >"$STUB_LOG"; : >"$VERIFY_LOG"
if RUNS=5 PIPE_CHUNK_VERIFY_SCRIPT="$TEST_ROOT/verify-fail" run_sweep "$OUT" --verify --out-rate=0.1 >"$TEST_ROOT/vfail.out" 2>"$TEST_ROOT/vfail.err"; then fail 'verification failure did not abort'; fi
assert_eq "$(count_lines "$VERIFY_LOG")" 1 'verify failure immediate abort'

# WSL cache-drop request is labeled and performs no sync or sudo/cache write.
OUT=$(new_out wsl-drop)
: >"$STUB_LOG"; : >"$SYNC_LOG"; : >"$SUDO_LOG"
RUNS=1 PIPE_CHUNK_PROC_VERSION_FILE="$FIXTURE_DIR/proc-wsl" run_sweep "$OUT" --cache=drop
[[ ! -s "$SYNC_LOG" && ! -s "$SUDO_LOG" ]] || fail 'WSL path attempted sync or sudo'
grep -q ',wsl-no-drop,' "$OUT/trials.csv" || fail 'WSL cache method not recorded'
OUT=$(new_out wsl-distro-env)
: >"$STUB_LOG"; : >"$SYNC_LOG"; : >"$SUDO_LOG"
RUNS=1 WSL_DISTRO_NAME=Ubuntu PIPE_CHUNK_PROC_VERSION_FILE="$FIXTURE_DIR/proc-linux" run_sweep "$OUT" --cache=drop
[[ ! -s "$SYNC_LOG" && ! -s "$SUDO_LOG" ]] || fail 'WSL_DISTRO_NAME path attempted sync or sudo'
grep -q ',wsl-no-drop,' "$OUT/trials.csv" || fail 'WSL_DISTRO_NAME cache method not recorded'

# Native drop: denied preflight, successful sudo tee to temp target, unreadable proc, sync and write failures.
OUT=$(new_out sudo-denied)
: >"$STUB_LOG"; : >"$SUDO_LOG"
if RUNS=1 SWEEP_TEST_UID=1000 SUDO_DENY=1 PIPE_CHUNK_PROC_VERSION_FILE="$FIXTURE_DIR/proc-linux" PIPE_CHUNK_DROP_CACHES_FILE="$FIXTURE_DIR/drop-caches" run_sweep "$OUT" --cache=drop >"$TEST_ROOT/sdeny.out" 2>"$TEST_ROOT/sdeny.err"; then fail 'denied sudo preflight succeeded'; fi
assert_eq "$(count_lines "$STUB_LOG")" 0 'denied sudo must stop before trials'
grep -q -- '-n true' "$SUDO_LOG" || fail 'sudo denial path was not reached'
OUT=$(new_out sudo-success)
: >"$STUB_LOG"; : >"$SUDO_LOG"; printf '3\n' >"$FIXTURE_DIR/drop-caches"
RUNS=1 SWEEP_TEST_UID=1000 PIPE_CHUNK_PROC_VERSION_FILE="$FIXTURE_DIR/proc-linux" PIPE_CHUNK_DROP_CACHES_FILE="$FIXTURE_DIR/drop-caches" run_sweep "$OUT" --cache=drop
assert_eq "$(count_lines "$SUDO_LOG")" 55 'sudo preflight, calibration drop, and one drop per trial'
assert_eq "$(cat "$FIXTURE_DIR/drop-caches")" 3 'sudo stub should write temporary drop target'
OUT=$(new_out unreadable-proc)
if RUNS=1 SWEEP_TEST_UID=1000 PIPE_CHUNK_PROC_VERSION_FILE="$TEST_ROOT/missing-proc" run_sweep "$OUT" --cache=drop >"$TEST_ROOT/proc.out" 2>"$TEST_ROOT/proc.err"; then fail 'unreadable proc version accepted'; fi
OUT=$(new_out sync-failure)
: >"$SYNC_LOG"
if RUNS=1 SWEEP_TEST_UID=1000 SYNC_FAIL=1 PIPE_CHUNK_PROC_VERSION_FILE="$FIXTURE_DIR/proc-linux" PIPE_CHUNK_DROP_CACHES_FILE="$FIXTURE_DIR/drop-caches" run_sweep "$OUT" --cache=drop >"$TEST_ROOT/sync.out" 2>"$TEST_ROOT/sync.err"; then fail 'sync failure accepted'; fi
OUT=$(new_out drop-failure)
if RUNS=1 SWEEP_TEST_UID=1000 SUDO_DROP_FAIL=1 PIPE_CHUNK_PROC_VERSION_FILE="$FIXTURE_DIR/proc-linux" PIPE_CHUNK_DROP_CACHES_FILE="$FIXTURE_DIR/drop-caches" run_sweep "$OUT" --cache=drop >"$TEST_ROOT/drop.out" 2>"$TEST_ROOT/drop.err"; then fail 'drop failure accepted'; fi

# Output path: nonexistent in-tree accepted; existing, outside-root, dot-dot, and symlink escape rejected.
OUT=$(new_out larger-rejected)
: >"$STUB_LOG"
if RUNS=1 run_sweep "$OUT" --cache=larger >"$TEST_ROOT/larger.out" 2>"$TEST_ROOT/larger.err"; then fail '--cache=larger accepted input not larger than MemTotal'; fi
assert_eq "$(count_lines "$STUB_LOG")" 0 '--cache=larger validation must precede trials'
OUT=$(new_out accepted-child)
: >"$STUB_LOG"
RUNS=1 run_sweep "$OUT"
assert_eq "$(count_lines "$STUB_LOG")" 53 'new in-tree output accepted'
mkdir "$TEST_ROOT/existing-empty"
expect_failure "$TEST_ROOT/existing-empty"
mkdir "$TEST_ROOT/existing-full"; : >"$TEST_ROOT/existing-full/sentinel"
expect_failure "$TEST_ROOT/existing-full"
expect_failure "$ROOT/bench/outside-results"
expect_failure "$RESULTS_ROOT/../escaped-result"
mkdir "$INPUT_ROOT/outside-target"
symlink_escape="$RESULTS_ROOT/.symlink-escape-$RANDOM"
ln -s "$INPUT_ROOT/outside-target" "$symlink_escape"
expect_failure "$symlink_escape/child"

printf 'test_sweep: all checks passed\n'
