#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"

# Pipeline CLI bindings: keep every flag and the verifier path together here.
readonly MODE_FLAG='--mode'
readonly BLOCK_SIZE_FLAG='--block-size'
readonly QUEUE_FLAG='--queue'
readonly WORKERS_FLAG='--workers'
readonly OUT_RATE_FLAG='--out-rate'
readonly METRICS_FLAG='--metrics'
readonly UNBOUNDED_FLAG='--unbounded'
readonly MANIFEST_FLAG='--manifest'
readonly VERIFY_SCRIPT_PATH="$REPO_ROOT/scripts/verify.sh"

# Named memory-comparison defaults required by W3-01.
readonly DEFAULT_MEMORY_QUEUE=4
readonly DEFAULT_MEMORY_WORKERS=4
readonly DEFAULT_MEMORY_BLOCK_SIZE=65536
readonly DEFAULT_OUT_RATE_MB_S=1
readonly TRIALS=5

RUNS_HEADER='run_group,mode,status,queue_cap,workers,block_bytes,trial,input_bytes,cache_method,out_rate_mb_s,bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s'
MEDIANS_HEADER='run_group,mode,queue_cap,workers,block_bytes,input_bytes,cache_method,out_rate_mb_s,bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s'
METRIC_COLUMNS=(bytes blocks wall_s throughput_mb_s lat_p50_us lat_p99_us cpu_user_s cpu_sys_s peak_rss_kb io_call_s)

usage() {
  cat <<'USAGE'
Usage: bench/sweep.sh --bin PATH --input PATH [--out-dir PATH]
       [--cache warm|larger|drop] [--verify]
       [--memory-queue N] [--memory-workers N] [--memory-block-size BYTES]
       [--out-rate MB_PER_S]

Runs the W3-01 core matrix, blocking baseline, and matched memory comparison.
Relative --out-dir paths are resolved from the repository root.
USAGE
}

fail() {
  printf 'sweep: %s\n' "$*" >&2
  exit 2
}

is_uint() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

strip_leading_zeroes() {
  local value="$1"
  while [[ "$value" == 0* && "$value" != 0 ]]; do
    value="${value#0}"
  done
  printf '%s' "$value"
}

is_positive_uint() {
  is_uint "$1" && [[ "$(strip_leading_zeroes "$1")" != 0 ]]
}

is_decimal() {
  [[ "$1" =~ ^[0-9]+([.][0-9]*)?([eE][+-]?[0-9]+)?$ ]]
}

is_metric_number() {
  [[ "$1" =~ ^-?[0-9]+([.][0-9]*)?([eE][+-]?[0-9]+)?$ ]]
}

get_caller_uid() {
  if [[ "${SWEEP_TEST_UID+x}" ]]; then
    is_uint "$SWEEP_TEST_UID" || return 1
    printf '%s\n' "$(strip_leading_zeroes "$SWEEP_TEST_UID")"
  else
    id -u
  fi
}

BIN_PATH=''
INPUT_PATH=''
REQUESTED_OUT_DIR=''
OUT_DIR_WAS_SET=0
CACHE_METHOD='warm'
VERIFY_ENABLED=0
MEMORY_QUEUE="$DEFAULT_MEMORY_QUEUE"
MEMORY_WORKERS="$DEFAULT_MEMORY_WORKERS"
MEMORY_BLOCK_SIZE="$DEFAULT_MEMORY_BLOCK_SIZE"
OUT_RATE_MB_S="$DEFAULT_OUT_RATE_MB_S"

need_value() {
  [[ $# -ge 2 && -n "$2" ]] || fail "missing value for $1"
}

while (($#)); do
  case "$1" in
    --bin)
      need_value "$1" "${2-}"
      BIN_PATH="$2"
      shift 2
      ;;
    --bin=*) BIN_PATH="${1#*=}"; shift ;;
    --input)
      need_value "$1" "${2-}"
      INPUT_PATH="$2"
      shift 2
      ;;
    --input=*) INPUT_PATH="${1#*=}"; shift ;;
    --out-dir)
      need_value "$1" "${2-}"
      REQUESTED_OUT_DIR="$2"
      OUT_DIR_WAS_SET=1
      shift 2
      ;;
    --out-dir=*) REQUESTED_OUT_DIR="${1#*=}"; OUT_DIR_WAS_SET=1; shift ;;
    --cache)
      need_value "$1" "${2-}"
      CACHE_METHOD="$2"
      shift 2
      ;;
    --cache=*) CACHE_METHOD="${1#*=}"; shift ;;
    --verify) VERIFY_ENABLED=1; shift ;;
    --memory-queue)
      need_value "$1" "${2-}"
      MEMORY_QUEUE="$2"
      shift 2
      ;;
    --memory-queue=*) MEMORY_QUEUE="${1#*=}"; shift ;;
    --memory-workers)
      need_value "$1" "${2-}"
      MEMORY_WORKERS="$2"
      shift 2
      ;;
    --memory-workers=*) MEMORY_WORKERS="${1#*=}"; shift ;;
    --memory-block-size)
      need_value "$1" "${2-}"
      MEMORY_BLOCK_SIZE="$2"
      shift 2
      ;;
    --memory-block-size=*) MEMORY_BLOCK_SIZE="${1#*=}"; shift ;;
    --out-rate)
      need_value "$1" "${2-}"
      OUT_RATE_MB_S="$2"
      shift 2
      ;;
    --out-rate=*) OUT_RATE_MB_S="${1#*=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) fail "unknown option: $1" ;;
  esac
done

[[ -n "$BIN_PATH" ]] || fail '--bin is required'
[[ -n "$INPUT_PATH" ]] || fail '--input is required'
[[ "$CACHE_METHOD" == warm || "$CACHE_METHOD" == larger || "$CACHE_METHOD" == drop ]] ||
  fail '--cache must be warm, larger, or drop'
[[ -f "$BIN_PATH" && -x "$BIN_PATH" ]] || fail "executable not found or not executable: $BIN_PATH"
[[ -f "$INPUT_PATH" && -r "$INPUT_PATH" ]] || fail "input is not a readable regular file: $INPUT_PATH"
BIN_PATH="$(realpath -e -- "$BIN_PATH")"
INPUT_PATH="$(realpath -e -- "$INPUT_PATH")"
INPUT_BYTES="$(stat -c '%s' -- "$INPUT_PATH")"
is_uint "$INPUT_BYTES" || fail 'could not determine input file size'

is_positive_uint "$MEMORY_QUEUE" || fail '--memory-queue must be a positive integer'
is_uint "$MEMORY_WORKERS" && (( 10#$MEMORY_WORKERS >= 1 && 10#$MEMORY_WORKERS <= 1024 )) ||
  fail '--memory-workers must be an integer from 1 through 1024'
is_uint "$MEMORY_BLOCK_SIZE" && (( 10#$MEMORY_BLOCK_SIZE >= 1 && 10#$MEMORY_BLOCK_SIZE <= 1073741824 )) ||
  fail '--memory-block-size must be an integer from 1 through 1073741824 bytes'
is_decimal "$OUT_RATE_MB_S" || fail '--out-rate must be a positive decimal number'
awk -v rate="$OUT_RATE_MB_S" 'BEGIN { exit !(rate + 0 > 0 && rate + 0 <= 1e9) }' ||
  fail '--out-rate must be greater than 0 and no greater than 1e9 MB/s'

TEST_MODE=0
if [[ "${PIPE_CHUNK_TESTING:-}" == 1 ]]; then
  TEST_MODE=1
fi
if (( ! TEST_MODE )) && { [[ "${PIPE_CHUNK_PROC_VERSION_FILE+x}" ]] ||
  [[ "${PIPE_CHUNK_DROP_CACHES_FILE+x}" ]] || [[ "${PIPE_CHUNK_VERIFY_SCRIPT+x}" ]]; }; then
  fail 'proc-version, drop-caches, and verifier path overrides require PIPE_CHUNK_TESTING=1'
fi
PROC_VERSION_FILE='/proc/version'
DROP_CACHES_FILE='/proc/sys/vm/drop_caches'
VERIFY_SCRIPT="$VERIFY_SCRIPT_PATH"
if (( TEST_MODE )); then
  PROC_VERSION_FILE="${PIPE_CHUNK_PROC_VERSION_FILE:-$PROC_VERSION_FILE}"
  DROP_CACHES_FILE="${PIPE_CHUNK_DROP_CACHES_FILE:-$DROP_CACHES_FILE}"
  VERIFY_SCRIPT="${PIPE_CHUNK_VERIFY_SCRIPT:-$VERIFY_SCRIPT}"
fi
if (( VERIFY_ENABLED )); then
  [[ -f "$VERIFY_SCRIPT" && -x "$VERIFY_SCRIPT" ]] || fail "verifier is not executable: $VERIFY_SCRIPT"
fi

if [[ "$CACHE_METHOD" == larger ]]; then
  MEM_TOTAL_KB="$(awk '$1 == "MemTotal:" { print $2; found = 1; exit } END { if (!found) exit 1 }' /proc/meminfo)" ||
    fail 'could not read MemTotal from /proc/meminfo'
  is_uint "$MEM_TOTAL_KB" || fail 'invalid MemTotal value in /proc/meminfo'
  if (( INPUT_BYTES <= MEM_TOTAL_KB * 1024 )); then
    fail "--cache=larger requires input_bytes greater than MemTotal_kB * 1024 ($((MEM_TOTAL_KB * 1024)))"
  fi
fi

CALLER_UID=''
if [[ "$CACHE_METHOD" == drop ]]; then
  [[ -r "$PROC_VERSION_FILE" ]] || fail "cannot read proc-version file: $PROC_VERSION_FILE"
  PROC_VERSION_CONTENT="$(cat -- "$PROC_VERSION_FILE" 2>/dev/null)" ||
    fail "cannot read proc-version file: $PROC_VERSION_FILE"
  PROC_VERSION_LOWER="${PROC_VERSION_CONTENT,,}"
  [[ "$PROC_VERSION_LOWER" != *microsoft* ]] || fail 'cache-drop mode is not supported under WSL'
  [[ "${WSL_DISTRO_NAME+x}" != x ]] || fail 'cache-drop mode is not supported when WSL_DISTRO_NAME is set'
  CALLER_UID="$(get_caller_uid)" || fail 'SWEEP_TEST_UID must be a nonnegative integer'
  if [[ "$CALLER_UID" != 0 ]]; then
    command -v sudo >/dev/null 2>&1 || fail 'cache-drop mode requires root or passwordless sudo -n'
    if ! sudo -n true; then
      fail 'cache-drop mode requires passwordless sudo -n; no cache-drop trial was started'
    fi
  fi
fi

RESULTS_ROOT="$(realpath -m -- "$SCRIPT_DIR/results")"
if (( OUT_DIR_WAS_SET )); then
  if [[ "$REQUESTED_OUT_DIR" == /* ]]; then
    OUT_DIR_CANDIDATE="$REQUESTED_OUT_DIR"
  else
    OUT_DIR_CANDIDATE="$REPO_ROOT/$REQUESTED_OUT_DIR"
  fi
else
  OUT_DIR_CANDIDATE="$RESULTS_ROOT/$(date -u '+%Y%m%dT%H%M%SZ')"
fi
OUT_DIR="$(realpath -m -- "$OUT_DIR_CANDIDATE")"
case "$OUT_DIR" in
  "$RESULTS_ROOT"/*) ;;
  *) fail "--out-dir must be a strict descendant of $RESULTS_ROOT" ;;
esac
if [[ -e "$OUT_DIR" || -L "$OUT_DIR" || -L "$OUT_DIR_CANDIDATE" ]]; then
  fail "result path already exists: $OUT_DIR"
fi

if ! mkdir -p -- "$(dirname -- "$OUT_DIR")"; then
  fail "could not create parent directory for $OUT_DIR"
fi
if ! mkdir -- "$OUT_DIR"; then
  fail "could not atomically create result directory: $OUT_DIR"
fi

SCRATCH_DIR="$(mktemp -d -- "${TMPDIR:-/tmp}/pipe-chunk-sweep.XXXXXX")"
cleanup() {
  if [[ -n "${SCRATCH_DIR:-}" && -d "$SCRATCH_DIR" ]]; then
    rm -rf -- "$SCRATCH_DIR"
  fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

RUNS_CSV="$OUT_DIR/runs.csv"
MEDIANS_CSV="$OUT_DIR/medians.csv"
printf '%s\n' "$RUNS_HEADER" >"$RUNS_CSV"
printf '%s\n' "$MEDIANS_HEADER" >"$MEDIANS_CSV"

declare -a CONFIGS=()
add_config() {
  CONFIGS+=("$1|$2|$3|$4|$5|$6|$7")
}

CORE_QUEUES=(1 4 16 64)
CORE_WORKERS=(1 2 4 8)
CORE_BLOCKS=(4096 65536 1048576)
for queue in "${CORE_QUEUES[@]}"; do
  for workers in "${CORE_WORKERS[@]}"; do
    for block_bytes in "${CORE_BLOCKS[@]}"; do
      add_config core pipeline "$queue" "$workers" "$block_bytes" '' no
    done
  done
done
for block_bytes in "${CORE_BLOCKS[@]}"; do
  add_config baseline blocking '' 1 "$block_bytes" '' no
done
add_config memory bounded "$MEMORY_QUEUE" "$MEMORY_WORKERS" "$MEMORY_BLOCK_SIZE" "$OUT_RATE_MB_S" no
add_config memory unbounded "$MEMORY_QUEUE" "$MEMORY_WORKERS" "$MEMORY_BLOCK_SIZE" "$OUT_RATE_MB_S" yes

declare -A GROUP_SUCCESS=()
declare -A GROUP_VALUES=()
overall_failure=0
run_number=0

drop_caches_before_trial() {
  if ! sync; then
    printf 'sweep: sync failed before drop-cache trial\n' >&2
    return 1
  fi
  if [[ "$CALLER_UID" == 0 ]]; then
    if ! printf '3\n' >"$DROP_CACHES_FILE"; then
      printf 'sweep: failed to write cache-drop target: %s\n' "$DROP_CACHES_FILE" >&2
      return 1
    fi
  else
    if ! sudo -n sh -c 'printf "3\\n" > "$1"' sh "$DROP_CACHES_FILE"; then
      printf 'sweep: privileged cache-drop write failed: %s\n' "$DROP_CACHES_FILE" >&2
      return 1
    fi
  fi
}

parse_summary() {
  local summary_path="$1"
  local wanted parsed value
  wanted='bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s'
  parsed="$(awk -F, -v wanted="$wanted" '
    NR == 1 {
      for (i = 1; i <= NF; i++) column[$i] = i
      count = split(wanted, name, ",")
      for (i = 1; i <= count; i++) if (!(name[i] in column)) bad = 1
      next
    }
    NR == 2 {
      if (bad) exit 1
      for (i = 1; i <= count; i++) {
        if (i > 1) printf ","
        printf "%s", $(column[name[i]])
      }
      print ",END"
      next
    }
    NR > 2 { bad = 1 }
    END { if (NR != 2 || bad) exit 1 }
  ' "$summary_path")" || return 1

  local -a values=()
  IFS=, read -r -a values <<<"$parsed"
  (( ${#values[@]} == 11 )) || return 1
  [[ "${values[10]}" == END ]] || return 1
  unset 'values[10]'
  for value in "${values[@]}"; do
    if [[ -n "$value" && "$value" != n/a ]] && ! is_metric_number "$value"; then
      return 1
    fi
  done
  METRIC_VALUES=("${values[@]}")
}

append_run_row() {
  local group="$1" mode="$2" status="$3" queue_cap="$4" workers="$5"
  local block_bytes="$6" trial="$7" rate="$8"
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$group" "$mode" "$status" "$queue_cap" "$workers" "$block_bytes" "$trial" \
    "$INPUT_BYTES" "$CACHE_METHOD" "$rate" \
    "${METRIC_VALUES[0]-}" "${METRIC_VALUES[1]-}" "${METRIC_VALUES[2]-}" \
    "${METRIC_VALUES[3]-}" "${METRIC_VALUES[4]-}" "${METRIC_VALUES[5]-}" \
    "${METRIC_VALUES[6]-}" "${METRIC_VALUES[7]-}" "${METRIC_VALUES[8]-}" \
    "${METRIC_VALUES[9]-}" >>"$RUNS_CSV"
}

for ((trial = 1; trial <= TRIALS; trial++)); do
  for config in "${CONFIGS[@]}"; do
    IFS='|' read -r group report_mode queue_cap workers block_bytes rate unbounded <<<"$config"
    run_number=$((run_number + 1))
    run_id="$(printf '%04d' "$run_number")"
    group_key="$group|$report_mode|$queue_cap|$workers|$block_bytes|$INPUT_BYTES|$CACHE_METHOD|$rate"
    output_path="$SCRATCH_DIR/output_$run_id"
    manifest_path="$output_path.manifest"
    metrics_prefix="$SCRATCH_DIR/metrics/run_$run_id"
    mkdir -p -- "$(dirname -- "$metrics_prefix")"

    METRIC_VALUES=()
    pipeline_ok=1
    summary_ok=0
    verify_ok=1
    cache_ok=1

    if [[ "$CACHE_METHOD" == drop ]] && ! drop_caches_before_trial; then
      cache_ok=0
      pipeline_ok=0
    else
      if [[ "$group" == baseline ]]; then
        command=("$BIN_PATH" "$MODE_FLAG" blocking "$BLOCK_SIZE_FLAG" "$block_bytes" \
          "$METRICS_FLAG" "$metrics_prefix" "$INPUT_PATH" "$output_path")
      elif [[ "$group" == core ]]; then
        command=("$BIN_PATH" "$MODE_FLAG" pipeline "$QUEUE_FLAG" "$queue_cap" \
          "$WORKERS_FLAG" "$workers" "$BLOCK_SIZE_FLAG" "$block_bytes" \
          "$METRICS_FLAG" "$metrics_prefix")
        if (( VERIFY_ENABLED && trial == 1 )); then
          command+=("$MANIFEST_FLAG" "$manifest_path")
        fi
        command+=("$INPUT_PATH" "$output_path")
      else
        command=("$BIN_PATH" "$MODE_FLAG" pipeline "$QUEUE_FLAG" "$queue_cap" \
          "$WORKERS_FLAG" "$workers" "$BLOCK_SIZE_FLAG" "$block_bytes" \
          "$OUT_RATE_FLAG" "$rate" "$METRICS_FLAG" "$metrics_prefix")
        if [[ "$unbounded" == yes ]]; then
          command+=("$UNBOUNDED_FLAG")
        fi
        command+=("$INPUT_PATH" "$output_path")
      fi

      if ! "${command[@]}"; then
        pipeline_ok=0
        printf 'sweep: %s trial %s command failed\n' "$group/$report_mode" "$trial" >&2
      fi

      if [[ -f "$metrics_prefix.summary.csv" ]]; then
        if parse_summary "$metrics_prefix.summary.csv"; then
          summary_ok=1
        else
          pipeline_ok=0
          printf 'sweep: invalid W2-06 summary for %s trial %s\n' "$group/$report_mode" "$trial" >&2
        fi
      else
        pipeline_ok=0
        printf 'sweep: missing %s.summary.csv for %s trial %s\n' \
          "$metrics_prefix" "$group/$report_mode" "$trial" >&2
      fi

      if (( VERIFY_ENABLED )) && [[ "$group" == core ]] && (( trial == 1 )); then
        if ! "$VERIFY_SCRIPT" "$INPUT_PATH" "$output_path" "$manifest_path"; then
          verify_ok=0
          printf 'sweep: verifier failed for core configuration (queue=%s workers=%s block=%s)\n' \
            "$queue_cap" "$workers" "$block_bytes" >&2
        fi
      fi
    fi

    status=success
    if (( ! pipeline_ok || ! summary_ok || ! verify_ok || ! cache_ok )); then
      status=failed
    fi

    if [[ "$status" == success && "$group" == core && "$trial" == 1 ]]; then
      occupancy_source="$metrics_prefix.occupancy.csv"
      occupancy_target="$OUT_DIR/occ_q${queue_cap}_w${workers}_b${block_bytes}_t1.csv"
      if [[ -f "$occupancy_source" ]]; then
        if ! cp -- "$occupancy_source" "$occupancy_target"; then
          status=failed
          printf 'sweep: could not retain occupancy file for core configuration\n' >&2
        fi
      else
        status=failed
        printf 'sweep: missing trial-1 occupancy CSV for core configuration\n' >&2
      fi
    fi

    append_run_row "$group" "$report_mode" "$status" "$queue_cap" "$workers" \
      "$block_bytes" "$trial" "$rate"

    if [[ "$status" == success ]]; then
      GROUP_SUCCESS["$group_key"]=$(( ${GROUP_SUCCESS[$group_key]:-0} + 1 ))
      for metric_index in "${!METRIC_COLUMNS[@]}"; do
        metric_value="${METRIC_VALUES[$metric_index]-}"
        if is_metric_number "$metric_value"; then
          metric_key="$group_key|${METRIC_COLUMNS[$metric_index]}"
          GROUP_VALUES["$metric_key"]="${GROUP_VALUES[$metric_key]:-}${metric_value} "
        fi
      done
    else
      overall_failure=1
    fi

    if (( ! cache_ok )); then
      printf 'sweep: aborting before pipeline invocation because cache-drop preparation failed\n' >&2
      exit 1
    fi
    if (( VERIFY_ENABLED )) && [[ "$group" == core ]] && (( trial == 1 )) && (( ! verify_ok )); then
      printf 'sweep: aborting after failed trial-1 verification\n' >&2
      exit 1
    fi
  done
done

declare -A GROUP_EMITTED=()
for config in "${CONFIGS[@]}"; do
  IFS='|' read -r group report_mode queue_cap workers block_bytes rate unbounded <<<"$config"
  group_key="$group|$report_mode|$queue_cap|$workers|$block_bytes|$INPUT_BYTES|$CACHE_METHOD|$rate"
  [[ "${GROUP_EMITTED[$group_key]+yes}" ]] && continue
  GROUP_EMITTED["$group_key"]=yes
  [[ "${GROUP_SUCCESS[$group_key]:-0}" == "$TRIALS" ]] || continue

  median_row="$group,$report_mode,$queue_cap,$workers,$block_bytes,$INPUT_BYTES,$CACHE_METHOD,$rate"
  for metric in "${METRIC_COLUMNS[@]}"; do
    metric_key="$group_key|$metric"
    metric_samples="${GROUP_VALUES[$metric_key]:-}"
    median_value=''
    if [[ -n "$metric_samples" ]]; then
      read -r -a sample_array <<<"$metric_samples"
      if (( ${#sample_array[@]} == TRIALS )); then
        sorted_samples="$(printf '%s\n' "${sample_array[@]}" | sort -n)"
        median_value="$(printf '%s\n' "$sorted_samples" | awk 'NR == 3 { print; exit }')"
      fi
    fi
    median_row+=",$median_value"
  done
  printf '%s\n' "$median_row" >>"$MEDIANS_CSV"
done

if (( overall_failure )); then
  printf 'sweep: one or more configuration groups had fewer than five successful trials; see %s\n' \
    "$RUNS_CSV" >&2
  exit 1
fi

printf 'sweep: completed 265 pipeline invocations; results: %s\n' "$OUT_DIR"
