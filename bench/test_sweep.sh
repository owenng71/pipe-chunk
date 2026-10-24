#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
SWEEP="$SCRIPT_DIR/sweep.sh"
RESULTS_ROOT="$(realpath -m -- "$SCRIPT_DIR/results")"

if [[ ! -x "$SWEEP" ]]; then
  printf 'test_sweep: expected executable %s; implement sweep.sh first\n' "$SWEEP" >&2
  exit 1
fi

unset PIPE_CHUNK_TESTING PIPE_CHUNK_PROC_VERSION_FILE PIPE_CHUNK_VERIFY_SCRIPT
unset PIPE_CHUNK_DROP_CACHES_FILE SWEEP_TEST_FAIL_FIRST_CORE SWEEP_TEST_UID
unset SWEEP_TEST_SUDO_LOG SWEEP_TEST_SUDO_MODE WSL_DISTRO_NAME

for tool in awk cat chmod dirname head ln mkdir mktemp realpath rm sort stat wc; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'test_sweep: required tool not found: %s\n' "$tool" >&2
    exit 1
  fi
done

mkdir -p -- "$RESULTS_ROOT"
TEST_ROOT="$(mktemp -d -- "$RESULTS_ROOT/.test_sweep.XXXXXX")"
OUTSIDE_ROOT="$(mktemp -d -- "${TMPDIR:-/tmp}/pipe-chunk-sweep-test.XXXXXX")"

cleanup() {
  rm -rf -- "$TEST_ROOT" "$OUTSIDE_ROOT"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

INPUT="$TEST_ROOT/input.fixture"
STUB="$TEST_ROOT/stub-pipe-chunk"
printf 'fixture input\n' >"$INPUT"

cat >"$STUB" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
: "${SWEEP_TEST_DIR:?}"
: "${SWEEP_TEST_LOG:?}"

mode=pipeline
queue=none
workers=none
block=none
rate=none
unbounded=no
prefix=
manifest=none
for ((i = 1; i <= $#; i++)); do
  arg="${!i}"
  case "$arg" in
    --mode=*) mode="${arg#*=}" ;;
    --mode) j=$((i + 1)); mode="${!j}" ;;
    --queue=*|--queue-cap=*) queue="${arg#*=}" ;;
    --queue|--queue-cap) j=$((i + 1)); queue="${!j}" ;;
    -q) j=$((i + 1)); queue="${!j}" ;;
    --workers=*) workers="${arg#*=}" ;;
    --workers|-w) j=$((i + 1)); workers="${!j}" ;;
    --block-size=*) block="${arg#*=}" ;;
    --block-size|-b) j=$((i + 1)); block="${!j}" ;;
    --out-rate=*) rate="${arg#*=}" ;;
    --out-rate) j=$((i + 1)); rate="${!j}" ;;
    --unbounded) unbounded=yes ;;
    --metrics=*) prefix="${arg#*=}" ;;
    --metrics) j=$((i + 1)); prefix="${!j}" ;;
    --manifest=*) manifest="${arg#*=}" ;;
    --manifest) j=$((i + 1)); manifest="${!j}" ;;
  esac
done

if [[ "$mode" == blocking ]]; then
  group=baseline
elif [[ "$rate" != none ]]; then
  group=memory
else
  group=core
fi
key="${group}_${mode}_${queue}_${workers}_${block}_${rate}_${unbounded}"
key="${key//[^a-zA-Z0-9_.-]/_}"
mkdir -p -- "$SWEEP_TEST_DIR/counters"
counter="$SWEEP_TEST_DIR/counters/$key"
count=0
if [[ -f "$counter" ]]; then
  read -r count <"$counter"
fi
count=$((count + 1))
printf '%s\n' "$count" >"$counter"
printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$group" "$mode" "$queue" "$workers" "$block" "$rate" "$unbounded" "$count" >>"$SWEEP_TEST_LOG"
printf '%s\n' "$manifest" >>"$SWEEP_TEST_DIR/manifest-args.log"

if [[ "${SWEEP_TEST_FAIL_FIRST_CORE:-0}" == 1 && "$group" == core && "$queue" == 1 && "$workers" == 1 && "$block" == 4096 && "$count" == 1 ]]; then
  exit 19
fi

if [[ -z "$prefix" ]]; then
  printf 'stub: sweep did not provide a metrics prefix\n' >&2
  exit 20
fi
mkdir -p -- "$(dirname -- "$prefix")"
if [[ -e "$prefix.summary.csv" || -e "$prefix.occupancy.csv" ]]; then
  printf 'stub: metrics prefix was reused: %s\n' "$prefix" >&2
  exit 21
fi
printf '%s\n' "$prefix" >>"$SWEEP_TEST_DIR/prefixes.log"

# Most configurations receive 10, 20, 30, 40, 50 across trials. The first
# core, baseline, and bounded-memory groups use intentionally unsorted samples
# whose numeric and lexical medians differ, so median selection is pinned.
value=$((count * 10))
lat_p50=$((value * 2))
peak_rss=64
if [[ "$count" -ge 1 && "$count" -le 5 ]] && {
  [[ "$group" == core && "$mode" == pipeline && "$queue" == 1 && "$workers" == 1 && "$block" == 4096 && "$rate" == none && "$unbounded" == no ]] ||
  [[ "$group" == baseline && "$mode" == blocking && "$block" == 4096 ]] ||
  [[ "$group" == memory && "$mode" == pipeline && "$queue" == 4 && "$workers" == 4 && "$block" == 65536 && "$rate" == 1 && "$unbounded" == no ]]
}; then
  case "$count" in
    1) value=9;   lat_p50=900;   peak_rss=90 ;;
    2) value=10;  lat_p50=1000;  peak_rss=100 ;;
    3) value=100; lat_p50=10000; peak_rss=1000 ;;
    4) value=2;   lat_p50=200;   peak_rss=20 ;;
    5) value=30;  lat_p50=3000;  peak_rss=300 ;;
  esac
fi
printf '%s\n' 'throughput_mb_s,io_call_s,bytes,lat_p99_us,blocks,wall_s,peak_rss_kb,lat_p50_us,cpu_sys_s,cpu_user_s' >"$prefix.summary.csv"
printf '%s\n' "$value,0.3,1000,$((value * 3)),10,1,$peak_rss,$lat_p50,0.2,0.1" >>"$prefix.summary.csv"
printf '%s\n' 't_ms,q1_size,q2_size' '0,0,0' '10,1,1' >"$prefix.occupancy.csv"
STUB
chmod +x -- "$STUB"

RUNS_HEADER='run_group,mode,status,queue_cap,workers,block_bytes,trial,input_bytes,cache_method,out_rate_mb_s,bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s'
MEDIANS_HEADER='run_group,mode,queue_cap,workers,block_bytes,input_bytes,cache_method,out_rate_mb_s,bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s'

new_run_context() {
  local name="$1"
  SWEEP_TEST_DIR="$TEST_ROOT/$name"
  mkdir -p -- "$SWEEP_TEST_DIR"
  SWEEP_TEST_LOG="$SWEEP_TEST_DIR/invocations.csv"
  : >"$SWEEP_TEST_LOG"
  export SWEEP_TEST_DIR SWEEP_TEST_LOG
}

expect_rejected_without_invocation() {
  local description="$1"
  local destination="$2"
  local before after
  before="$(wc -l <"$SWEEP_TEST_LOG")"
  if "$SWEEP" --bin "$STUB" --input "$INPUT" --cache=warm --out-dir "$destination"; then
    printf 'test_sweep: expected rejection for %s\n' "$description" >&2
    exit 1
  fi
  after="$(wc -l <"$SWEEP_TEST_LOG")"
  if [[ "$before" != "$after" ]]; then
    printf 'test_sweep: ran the executable before rejecting %s\n' "$description" >&2
    exit 1
  fi
}

assert_group_count() {
  local log="$1" group="$2" expected="$3" actual
  actual="$(awk -F, -v group="$group" '$1 == group { n++ } END { print n + 0 }' "$log")"
  if [[ "$actual" != "$expected" ]]; then
    printf 'test_sweep: expected %s %s invocations, got %s\n' "$expected" "$group" "$actual" >&2
    exit 1
  fi
}

assert_trial_major_order() {
  local log="$1"
  awk -F, '
    $8 < last_trial { bad = 1 }
    $8 > last_trial { last_trial = $8 }
    END { exit bad }
  ' "$log"
}

assert_baseline_flags() {
  local log="$1"
  awk -F, '$1 == "baseline" && ($3 != "none" || $4 != "none" || $6 != "none" || $7 != "no") { bad = 1 }
    END { exit bad }' "$log"
}

assert_memory_flag_pairing() {
  local log="$1"
  awk -F, '
    $1 == "memory" {
      if ($2 != "pipeline" || $3 == "none" || $4 == "none" || $5 == "none" || $6 == "none") bad = 1
      if ($7 == "no") bounded[$3 ":" $4 ":" $5 ":" $6]++
      else if ($7 == "yes") unbounded[$3 ":" $4 ":" $5 ":" $6]++
      else bad = 1
    }
    END {
      for (key in bounded) if (bounded[key] != 5 || unbounded[key] != 5) bad = 1
      for (key in unbounded) if (unbounded[key] != 5 || bounded[key] != 5) bad = 1
      exit bad
    }
  ' "$log"
}

assert_median_metrics() {
  local file="$1" group="$2" mode="$3" queue="$4" workers="$5" block="$6"
  local expected_throughput="$7" expected_lat_p50="$8" expected_peak_rss="$9"
  awk -F, -v group="$group" -v mode="$mode" -v queue="$queue" \
    -v workers="$workers" -v block="$block" \
    -v throughput="$expected_throughput" -v lat_p50="$expected_lat_p50" \
    -v peak_rss="$expected_peak_rss" '
    NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
    $(col["run_group"]) == group && $(col["mode"]) == mode &&
      $(col["queue_cap"]) == queue && $(col["workers"]) == workers &&
      $(col["block_bytes"]) == block {
        matches++
        if ($(col["throughput_mb_s"]) + 0 != throughput + 0 ||
            $(col["lat_p50_us"]) + 0 != lat_p50 + 0 ||
            $(col["peak_rss_kb"]) + 0 != peak_rss + 0) bad = 1
      }
    END { exit (matches == 1 && !bad) ? 0 : 1 }
  ' "$file"
}

assert_successful_group_count() {
  local file="$1" group="$2" mode="$3" queue="$4" workers="$5" block="$6" expected="$7"
  awk -F, -v group="$group" -v mode="$mode" -v queue="$queue" \
    -v workers="$workers" -v block="$block" -v expected="$expected" '
    NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
    $(col["run_group"]) == group && $(col["mode"]) == mode &&
      $(col["queue_cap"]) == queue && $(col["workers"]) == workers &&
      $(col["block_bytes"]) == block && $(col["status"]) == "success" { count++ }
    END { exit (count + 0 == expected + 0) ? 0 : 1 }
  ' "$file"
}

assert_no_median_group() {
  local file="$1" group="$2" mode="$3" queue="$4" workers="$5" block="$6"
  awk -F, -v group="$group" -v mode="$mode" -v queue="$queue" \
    -v workers="$workers" -v block="$block" '
    NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
    $(col["run_group"]) == group && $(col["mode"]) == mode &&
      $(col["queue_cap"]) == queue && $(col["workers"]) == workers &&
      $(col["block_bytes"]) == block { found = 1 }
    END { exit found }
  ' "$file"
}

assert_failed_row() {
  local file="$1"
  awk -F, '
    NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
    $(col["status"]) == "failed" { found = 1 }
    END { exit !found }
  ' "$file"
}

assert_memory_settings() {
  local file="$1" expected_queue="$2" expected_workers="$3" expected_block="$4" expected_rate="$5"
  awk -F, -v q="$expected_queue" -v w="$expected_workers" -v b="$expected_block" -v r="$expected_rate" '
    NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
    $(col["run_group"]) == "memory" && $(col["status"]) == "success" {
      if ($(col["workers"]) != w || $(col["block_bytes"]) != b || $(col["out_rate_mb_s"]) != r) bad = 1
      if ($(col["mode"]) == "bounded") bounded++
      else if ($(col["mode"]) == "unbounded") unbounded++
      else bad = 1
      if ($(col["queue_cap"]) != q) bad = 1
      n++
    }
    END { exit (n == 10 && bounded == 5 && unbounded == 5 && !bad) ? 0 : 1 }
  ' "$file"
}

assert_run_metadata() {
  local file="$1" expected_cache="$2" expected_bytes
  expected_bytes="$(stat -c '%s' -- "$INPUT")"
  awk -F, -v expected_cache="$expected_cache" -v expected_bytes="$expected_bytes" '
    NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
    $(col["input_bytes"]) != expected_bytes || $(col["cache_method"]) != expected_cache { bad = 1 }
    END { exit (NR > 1 && !bad) ? 0 : 1 }
  ' "$file"
}

assert_successful_verifier_scope() {
  local dir="$1" verify_log="$2" line_number=0 manifest_arg output_path manifest_path
  [[ "$(wc -l <"$verify_log")" == 48 ]] || {
    printf 'test_sweep: expected 48 successful verifier calls\n' >&2
    return 1
  }
  awk -F, 'NR <= 48 && ($1 != "core" || $8 != 1) { bad = 1 } END { exit bad }' "$SWEEP_TEST_LOG" || {
    printf 'test_sweep: trial-major prefix did not contain the 48 core trial-1 runs\n' >&2
    return 1
  }
  while IFS=, read -r _ output_path manifest_path; do
    line_number=$((line_number + 1))
    [[ "${output_path##*/}" == "output_$(printf '%04d' "$line_number")" ]] || return 1
    [[ "$manifest_path" == "$output_path.manifest" ]] || return 1
  done <"$verify_log"
  [[ "$(wc -l <"$dir/manifest-args.log")" == 265 ]] || return 1
  line_number=0
  while IFS= read -r manifest_arg; do
    line_number=$((line_number + 1))
    if (( line_number <= 48 )); then
      [[ "${manifest_arg##*/}" == "output_$(printf '%04d' "$line_number").manifest" ]] || return 1
    else
      [[ "$manifest_arg" == none ]] || return 1
    fi
  done <"$dir/manifest-args.log"
}

assert_path_rejections() {
  local empty_dir="$TEST_ROOT/existing-empty"
  local full_dir="$TEST_ROOT/existing-nonempty"
  local outside="$OUTSIDE_ROOT/outside-result"
  local traversal="$SCRIPT_DIR/results/../.test-sweep-escape-$$"
  local symlink="$TEST_ROOT/symlink-escape/new-result"

  mkdir -- "$empty_dir" "$full_dir"
  printf 'preserve me\n' >"$full_dir/sentinel"
  ln -s -- "$OUTSIDE_ROOT" "$TEST_ROOT/symlink-escape"

  expect_rejected_without_invocation 'existing empty directory' "$empty_dir"
  expect_rejected_without_invocation 'existing non-empty directory' "$full_dir"
  [[ -f "$full_dir/sentinel" ]] || { printf 'test_sweep: existing content was changed\n' >&2; exit 1; }
  expect_rejected_without_invocation 'outside results root' "$outside"
  expect_rejected_without_invocation '.. traversal escape' "$traversal"
  expect_rejected_without_invocation 'symlink escape' "$symlink"
  [[ ! -e "$outside" && ! -e "$traversal" && ! -e "$symlink" ]] || {
    printf 'test_sweep: a rejected destination was created\n' >&2
    exit 1
  }
}

new_run_context path-checks
assert_path_rejections

VERIFY_SUCCESS="$TEST_ROOT/successful-verify"
cat >"$VERIFY_SUCCESS" <<'VERIFY'
#!/usr/bin/env bash
set -euo pipefail
: "${SWEEP_TEST_DIR:?}"
printf '%s,%s,%s\n' "$1" "$2" "$3" >>"$SWEEP_TEST_DIR/verify.log"
VERIFY
chmod +x -- "$VERIFY_SUCCESS"

new_run_context default
ACCEPTED_DIR="$TEST_ROOT/new-parent/accepted-run"
if ! PIPE_CHUNK_TESTING=1 PIPE_CHUNK_VERIFY_SCRIPT="$VERIFY_SUCCESS" \
  "$SWEEP" --bin "$STUB" --input "$INPUT" --cache=warm --out-dir "$ACCEPTED_DIR" --verify; then
  printf 'test_sweep: nonexistent in-tree --out-dir was not accepted\n' >&2
  exit 1
fi
[[ -d "$ACCEPTED_DIR" ]] || { printf 'test_sweep: accepted result directory was not created\n' >&2; exit 1; }

[[ "$(head -n 1 "$ACCEPTED_DIR/runs.csv")" == "$RUNS_HEADER" ]] || {
  printf 'test_sweep: runs.csv header does not match the W3-01 schema\n' >&2
  exit 1
}
[[ "$(head -n 1 "$ACCEPTED_DIR/medians.csv")" == "$MEDIANS_HEADER" ]] || {
  printf 'test_sweep: medians.csv header does not match the W3-01 schema\n' >&2
  exit 1
}
assert_group_count "$SWEEP_TEST_LOG" core 240
assert_group_count "$SWEEP_TEST_LOG" baseline 15
assert_group_count "$SWEEP_TEST_LOG" memory 10
assert_trial_major_order "$SWEEP_TEST_LOG" || {
  printf 'test_sweep: invocations were not ordered trial-major\n' >&2
  exit 1
}
assert_baseline_flags "$SWEEP_TEST_LOG" || {
  printf 'test_sweep: baseline received queue, worker, rate, or unbounded flags\n' >&2
  exit 1
}
assert_memory_flag_pairing "$SWEEP_TEST_LOG" || {
  printf 'test_sweep: bounded/unbounded runs did not use matching flags except --unbounded\n' >&2
  exit 1
}
[[ "$(wc -l <"$ACCEPTED_DIR/runs.csv")" == 266 ]] || {
  printf 'test_sweep: runs.csv does not contain one row per attempt\n' >&2
  exit 1
}
[[ "$(wc -l <"$ACCEPTED_DIR/medians.csv")" == 54 ]] || {
  printf 'test_sweep: medians.csv does not contain all 53 complete groups\n' >&2
  exit 1
}
assert_median_metrics "$ACCEPTED_DIR/medians.csv" core pipeline 1 1 4096 10 1000 100 || {
  printf 'test_sweep: exact numeric medians for the first core group were incorrect\n' >&2
  exit 1
}
assert_median_metrics "$ACCEPTED_DIR/medians.csv" baseline blocking '' 1 4096 10 1000 100 || {
  printf 'test_sweep: exact numeric medians for the first baseline group were incorrect\n' >&2
  exit 1
}
assert_median_metrics "$ACCEPTED_DIR/medians.csv" memory bounded 4 4 65536 10 1000 100 || {
  printf 'test_sweep: exact numeric medians for the bounded memory group were incorrect\n' >&2
  exit 1
}
assert_median_metrics "$ACCEPTED_DIR/medians.csv" memory unbounded 4 4 65536 30 60 64 || {
  printf 'test_sweep: fixture median for the unbounded memory group was incorrect\n' >&2
  exit 1
}
assert_memory_settings "$ACCEPTED_DIR/runs.csv" 4 4 65536 1 || {
  printf 'test_sweep: memory comparison defaults were not recorded correctly\n' >&2
  exit 1
}
assert_run_metadata "$ACCEPTED_DIR/runs.csv" warm || {
  printf 'test_sweep: input_bytes or cache_method was wrong on a run row\n' >&2
  exit 1
}
assert_successful_verifier_scope "$SWEEP_TEST_DIR" "$SWEEP_TEST_DIR/verify.log" || {
  printf 'test_sweep: successful verifier calls did not match the trial-1 core scope\n' >&2
  exit 1
}
[[ "$(sort -u "$SWEEP_TEST_DIR/prefixes.log" | wc -l)" == 265 ]] || {
  printf 'test_sweep: metrics prefixes were not unique\n' >&2
  exit 1
}
shopt -s nullglob
occupancy_files=("$ACCEPTED_DIR"/occ_q*_w*_b*_t1.csv)
[[ "${#occupancy_files[@]}" == 48 ]] || {
  printf 'test_sweep: expected 48 retained core trial-1 occupancy files, got %s\n' "${#occupancy_files[@]}" >&2
  exit 1
}
other_occupancy_files=("$ACCEPTED_DIR"/*.occupancy.csv)
[[ "${#other_occupancy_files[@]}" == 0 ]] || {
  printf 'test_sweep: non-retained occupancy files remained in the result directory\n' >&2
  exit 1
}
while IFS= read -r prefix; do
  [[ ! -e "$prefix.summary.csv" && ! -e "$prefix.occupancy.csv" ]] || {
    printf 'test_sweep: per-run metrics scratch files were not removed\n' >&2
    exit 1
  }
done <"$SWEEP_TEST_DIR/prefixes.log"

new_run_context override
OVERRIDE_DIR="$TEST_ROOT/override-run"
SUDO_BIN_DIR="$TEST_ROOT/sudo-bin"
SUDO_STUB="$SUDO_BIN_DIR/sudo"
mkdir -p -- "$SUDO_BIN_DIR"
cat >"$SUDO_STUB" <<'SUDO'
#!/usr/bin/env bash
set -euo pipefail
: "${SWEEP_TEST_SUDO_LOG:?}"
printf '%s\n' "$*" >>"$SWEEP_TEST_SUDO_LOG"
[[ "${1-}" == -n ]] || exit 2
if [[ "${2-}" == true ]]; then
  [[ "${SWEEP_TEST_SUDO_MODE:-success}" != deny ]]
  exit
fi
if [[ "${2-}" == sh && "${3-}" == -c && "${5-}" == sh && -n "${6-}" ]]; then
  [[ "${SWEEP_TEST_SUDO_MODE:-success}" != write-deny ]] || exit 1
  printf '3\n' >"$6"
  exit 0
fi
exit 3
SUDO
chmod +x -- "$SUDO_STUB"
NATIVE_VERSION="$TEST_ROOT/proc-version-native"
printf 'Linux version fixture without WSL marker\n' >"$NATIVE_VERSION"
DROP_TARGET="$TEST_ROOT/drop-caches"
printf '0\n' >"$DROP_TARGET"
if ! PIPE_CHUNK_TESTING=1 PIPE_CHUNK_PROC_VERSION_FILE="$NATIVE_VERSION" \
  PIPE_CHUNK_DROP_CACHES_FILE="$DROP_TARGET" SWEEP_TEST_UID=12345 \
  SWEEP_TEST_SUDO_LOG="$SWEEP_TEST_DIR/sudo.log" SWEEP_TEST_SUDO_MODE=success \
  PATH="$SUDO_BIN_DIR:$PATH" \
  "$SWEEP" --bin "$STUB" --input "$INPUT" --cache=drop --out-dir "$OVERRIDE_DIR" \
  --memory-queue 8 --memory-workers 2 --memory-block-size 4096 --out-rate 2; then
  printf 'test_sweep: memory comparison overrides were rejected\n' >&2
  exit 1
fi
assert_memory_settings "$OVERRIDE_DIR/runs.csv" 8 2 4096 2 || {
  printf 'test_sweep: overridden memory settings were not recorded correctly\n' >&2
  exit 1
}
assert_run_metadata "$OVERRIDE_DIR/runs.csv" drop || {
  printf 'test_sweep: drop cache metadata was not recorded on every run row\n' >&2
  exit 1
}
[[ "$(wc -l <"$SWEEP_TEST_DIR/sudo.log")" == 266 ]] || {
  printf 'test_sweep: expected one sudo preflight and 265 cache-drop writes\n' >&2
  exit 1
}
[[ "$(awk '$1 == "-n" && $2 == "true" { n++ } END { print n + 0 }' "$SWEEP_TEST_DIR/sudo.log")" == 1 ]] || {
  printf 'test_sweep: successful sudo -n preflight was not observed\n' >&2
  exit 1
}
[[ "$(awk '$1 == "-n" && $2 == "sh" && $3 == "-c" { n++ } END { print n + 0 }' "$SWEEP_TEST_DIR/sudo.log")" == 265 ]] || {
  printf 'test_sweep: non-root cache-drop writes did not use sudo -n\n' >&2
  exit 1
}
[[ "$(cat "$DROP_TARGET")" == 3 ]] || {
  printf 'test_sweep: successful sudo stub did not write the temporary drop target\n' >&2
  exit 1
}

new_run_context failure
FAILED_GROUP_DIR="$TEST_ROOT/failure-run"
if SWEEP_TEST_FAIL_FIRST_CORE=1 "$SWEEP" --bin "$STUB" --input "$INPUT" --cache=warm --out-dir "$FAILED_GROUP_DIR"; then
  printf 'test_sweep: expected an incomplete group to produce a nonzero exit\n' >&2
  exit 1
fi
assert_group_count "$SWEEP_TEST_LOG" core 240
assert_group_count "$SWEEP_TEST_LOG" baseline 15
assert_group_count "$SWEEP_TEST_LOG" memory 10
assert_failed_row "$FAILED_GROUP_DIR/runs.csv" || {
  printf 'test_sweep: failed attempt was not flagged in runs.csv\n' >&2
  exit 1
}
assert_run_metadata "$FAILED_GROUP_DIR/runs.csv" warm || {
  printf 'test_sweep: failed-group rows lost input_bytes or cache_method\n' >&2
  exit 1
}
assert_successful_group_count "$FAILED_GROUP_DIR/runs.csv" core pipeline 1 1 4096 4 || {
  printf 'test_sweep: incomplete core group did not have exactly four successful trials\n' >&2
  exit 1
}
[[ "$(wc -l <"$FAILED_GROUP_DIR/runs.csv")" == 266 ]] || {
  printf 'test_sweep: ordinary group failure did not preserve all attempt rows\n' >&2
  exit 1
}
assert_no_median_group "$FAILED_GROUP_DIR/medians.csv" core pipeline 1 1 4096 || {
  printf 'test_sweep: incomplete core group received a median\n' >&2
  exit 1
}
[[ "$(wc -l <"$FAILED_GROUP_DIR/medians.csv")" == 53 ]] || {
  printf 'test_sweep: complete groups did not receive their medians after a group failure\n' >&2
  exit 1
}

VERIFY_STUB="$TEST_ROOT/failing-verify"
cat >"$VERIFY_STUB" <<'VERIFY'
#!/usr/bin/env bash
set -euo pipefail
count=0
if [[ -f "$SWEEP_TEST_DIR/verify-count" ]]; then
  read -r count <"$SWEEP_TEST_DIR/verify-count"
fi
count=$((count + 1))
printf '%s\n' "$count" >"$SWEEP_TEST_DIR/verify-count"
[[ "$count" != 2 ]]
VERIFY
chmod +x -- "$VERIFY_STUB"
new_run_context verify
VERIFY_DIR="$TEST_ROOT/verify-run"
if PIPE_CHUNK_TESTING=1 PIPE_CHUNK_VERIFY_SCRIPT="$VERIFY_STUB" \
  "$SWEEP" --bin "$STUB" --input "$INPUT" --cache=warm --out-dir "$VERIFY_DIR" --verify; then
  printf 'test_sweep: expected trial-1 verification failure to abort nonzero\n' >&2
  exit 1
fi
[[ "$(wc -l <"$SWEEP_TEST_LOG")" == 2 ]] || {
  printf 'test_sweep: verification failure did not abort immediately\n' >&2
  exit 1
}
[[ "$(wc -l <"$VERIFY_DIR/runs.csv")" == 3 ]] || {
  printf 'test_sweep: result rows written before verification failure were not preserved\n' >&2
  exit 1
}
assert_failed_row "$VERIFY_DIR/runs.csv" || {
  printf 'test_sweep: verification failure row was not preserved\n' >&2
  exit 1
}
assert_run_metadata "$VERIFY_DIR/runs.csv" warm || {
  printf 'test_sweep: verification failure rows lost input_bytes or cache_method\n' >&2
  exit 1
}
[[ "$(wc -l <"$VERIFY_DIR/medians.csv")" == 1 ]] || {
  printf 'test_sweep: incomplete verification-aborted sweep emitted medians\n' >&2
  exit 1
}

new_run_context wsl
WSL_VERSION="$TEST_ROOT/proc-version-wsl"
printf 'Linux version fixture with MiCrOsOfT marker\n' >"$WSL_VERSION"
WSL_DIR="$TEST_ROOT/wsl-run"
if WSL_MESSAGE="$(PIPE_CHUNK_TESTING=1 PIPE_CHUNK_PROC_VERSION_FILE="$WSL_VERSION" \
  "$SWEEP" --bin "$STUB" --input "$INPUT" --cache=drop --out-dir "$WSL_DIR" 2>&1)"; then
  printf 'test_sweep: expected drop mode to be refused under simulated WSL\n' >&2
  exit 1
fi
if [[ "${WSL_MESSAGE,,}" != *wsl* ]]; then
  printf 'test_sweep: WSL refusal did not explain the platform restriction\n' >&2
  exit 1
fi
[[ "$(wc -l <"$SWEEP_TEST_LOG")" == 0 && ! -e "$WSL_DIR" ]] || {
  printf 'test_sweep: WSL cache-drop refusal launched a run or created a result directory\n' >&2
  exit 1
}

new_run_context wsl-distro-name
WSL_ENV_DIR="$TEST_ROOT/wsl-env-run"
if WSL_ENV_MESSAGE="$(PIPE_CHUNK_TESTING=1 PIPE_CHUNK_PROC_VERSION_FILE="$NATIVE_VERSION" \
  WSL_DISTRO_NAME=Ubuntu-24.04 "$SWEEP" --bin "$STUB" --input "$INPUT" \
  --cache=drop --out-dir "$WSL_ENV_DIR" 2>&1)"; then
  printf 'test_sweep: expected WSL_DISTRO_NAME to refuse drop mode\n' >&2
  exit 1
fi
[[ "${WSL_ENV_MESSAGE,,}" == *wsl* && "$(wc -l <"$SWEEP_TEST_LOG")" == 0 && ! -e "$WSL_ENV_DIR" ]] || {
  printf 'test_sweep: WSL_DISTRO_NAME refusal was not clear or ran a trial\n' >&2
  exit 1
}

new_run_context larger
LARGER_DIR="$TEST_ROOT/larger-input-run"
if "$SWEEP" --bin "$STUB" --input "$INPUT" --cache=larger --out-dir "$LARGER_DIR"; then
  printf 'test_sweep: expected a small input to be refused for --cache=larger\n' >&2
  exit 1
fi
[[ "$(wc -l <"$SWEEP_TEST_LOG")" == 0 && ! -e "$LARGER_DIR" ]] || {
  printf 'test_sweep: --cache=larger launched a run or created a result directory\n' >&2
  exit 1
}

new_run_context unreadable-proc-version
UNREADABLE_DIR="$TEST_ROOT/unreadable-proc-version-run"
if UNREADABLE_MESSAGE="$(PIPE_CHUNK_TESTING=1 \
  PIPE_CHUNK_PROC_VERSION_FILE="$TEST_ROOT/no-such-proc-version" \
  "$SWEEP" --bin "$STUB" --input "$INPUT" --cache=drop --out-dir "$UNREADABLE_DIR" 2>&1)"; then
  printf 'test_sweep: expected an unreadable proc-version path to refuse drop mode\n' >&2
  exit 1
fi
[[ "${UNREADABLE_MESSAGE,,}" == *proc-version* && "$(wc -l <"$SWEEP_TEST_LOG")" == 0 && ! -e "$UNREADABLE_DIR" ]] || {
  printf 'test_sweep: unreadable proc-version refusal was not clear or ran a trial\n' >&2
  exit 1
}

new_run_context denied-sudo
DENIED_DIR="$TEST_ROOT/denied-sudo-run"
DENIED_SUDO_LOG="$SWEEP_TEST_DIR/sudo.log"
if DENIED_MESSAGE="$(PIPE_CHUNK_TESTING=1 PIPE_CHUNK_PROC_VERSION_FILE="$NATIVE_VERSION" \
  PIPE_CHUNK_DROP_CACHES_FILE="$DROP_TARGET" SWEEP_TEST_UID=12345 \
  SWEEP_TEST_SUDO_LOG="$DENIED_SUDO_LOG" SWEEP_TEST_SUDO_MODE=deny \
  PATH="$SUDO_BIN_DIR:$PATH" "$SWEEP" --bin "$STUB" --input "$INPUT" \
  --cache=drop --out-dir "$DENIED_DIR" 2>&1)"; then
  printf 'test_sweep: expected denied sudo -n preflight to fail\n' >&2
  exit 1
fi
[[ "${DENIED_MESSAGE,,}" == *sudo* && "$(wc -l <"$DENIED_SUDO_LOG")" == 1 ]] || {
  printf 'test_sweep: denied sudo preflight was not logged or reported\n' >&2
  exit 1
}
[[ "$(awk '$1 == "-n" && $2 == "true" { n++ } END { print n + 0 }' "$DENIED_SUDO_LOG")" == 1 ]] || {
  printf 'test_sweep: denied sudo stub did not observe the sudo -n call\n' >&2
  exit 1
}
[[ "$(wc -l <"$SWEEP_TEST_LOG")" == 0 && ! -e "$DENIED_DIR" ]] || {
  printf 'test_sweep: denied sudo preflight launched a pipeline run\n' >&2
  exit 1
}

SYNC_STUB_DIR="$TEST_ROOT/sync-failure-bin"
mkdir -p -- "$SYNC_STUB_DIR"
cat >"$SYNC_STUB_DIR/sync" <<'SYNC'
#!/usr/bin/env bash
exit 1
SYNC
chmod +x -- "$SYNC_STUB_DIR/sync"
new_run_context failed-sync
SYNC_DIR="$TEST_ROOT/failed-sync-run"
if PIPE_CHUNK_TESTING=1 PIPE_CHUNK_PROC_VERSION_FILE="$NATIVE_VERSION" \
  PIPE_CHUNK_DROP_CACHES_FILE="$DROP_TARGET" SWEEP_TEST_UID=12345 \
  SWEEP_TEST_SUDO_LOG="$SWEEP_TEST_DIR/sudo.log" SWEEP_TEST_SUDO_MODE=success \
  PATH="$SYNC_STUB_DIR:$SUDO_BIN_DIR:$PATH" \
  "$SWEEP" --bin "$STUB" --input "$INPUT" --cache=drop --out-dir "$SYNC_DIR" \
  >/dev/null 2>&1; then
  printf 'test_sweep: expected sync failure to abort drop mode\n' >&2
  exit 1
fi
[[ "$(wc -l <"$SYNC_DIR/runs.csv")" == 2 && "$(wc -l <"$SWEEP_TEST_LOG")" == 0 ]] || {
  printf 'test_sweep: sync failure did not record one failed row before any invocation\n' >&2
  exit 1
}
assert_failed_row "$SYNC_DIR/runs.csv" || exit 1
assert_run_metadata "$SYNC_DIR/runs.csv" drop || exit 1
[[ "$(awk '$1 == "-n" && $2 == "true" { n++ } END { print n + 0 }' "$SWEEP_TEST_DIR/sudo.log")" == 1 ]] || {
  printf 'test_sweep: sync-failure case did not pass sudo preflight before sync\n' >&2
  exit 1
}
[[ "$(awk '$1 == "-n" && $2 == "sh" && $3 == "-c" { n++ } END { print n + 0 }' "$SWEEP_TEST_DIR/sudo.log")" == 0 ]] || {
  printf 'test_sweep: cache-drop write ran after sync failed\n' >&2
  exit 1
}

new_run_context failed-drop-write
DROP_FAIL_DIR="$TEST_ROOT/failed-drop-write-run"
if PIPE_CHUNK_TESTING=1 PIPE_CHUNK_PROC_VERSION_FILE="$NATIVE_VERSION" \
  PIPE_CHUNK_DROP_CACHES_FILE="$DROP_TARGET" SWEEP_TEST_UID=12345 \
  SWEEP_TEST_SUDO_LOG="$SWEEP_TEST_DIR/sudo.log" SWEEP_TEST_SUDO_MODE=write-deny \
  PATH="$SUDO_BIN_DIR:$PATH" "$SWEEP" --bin "$STUB" --input "$INPUT" \
  --cache=drop --out-dir "$DROP_FAIL_DIR" >/dev/null 2>&1; then
  printf 'test_sweep: expected privileged drop-write failure to abort\n' >&2
  exit 1
fi
[[ "$(wc -l <"$DROP_FAIL_DIR/runs.csv")" == 2 && "$(wc -l <"$SWEEP_TEST_LOG")" == 0 ]] || {
  printf 'test_sweep: drop-write failure launched a pipeline run or omitted its failed row\n' >&2
  exit 1
}
assert_failed_row "$DROP_FAIL_DIR/runs.csv" || exit 1
assert_run_metadata "$DROP_FAIL_DIR/runs.csv" drop || exit 1
[[ "$(awk '$1 == "-n" && $2 == "true" { n++ } END { print n + 0 }' "$SWEEP_TEST_DIR/sudo.log")" == 1 ]] || {
  printf 'test_sweep: drop-write failure did not reach the successful sudo preflight\n' >&2
  exit 1
}
[[ "$(awk '$1 == "-n" && $2 == "sh" && $3 == "-c" { n++ } END { print n + 0 }' "$SWEEP_TEST_DIR/sudo.log")" == 1 ]] || {
  printf 'test_sweep: drop-write failure did not reach the privileged write\n' >&2
  exit 1
}

printf 'test_sweep: output path, schema, median, cache, verifier, and failure checks passed\n'
