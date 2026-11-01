#!/usr/bin/env bash
set -euo pipefail

# Pipeline interface spellings and verifier path stay together so this driver tracks the
# executable contract in one place.
PIPE_FLAG_BLOCK_SIZE='--block-size'
PIPE_FLAG_QUEUE='--queue'
PIPE_FLAG_WORKERS='--workers'
PIPE_FLAG_MODE='--mode'
PIPE_FLAG_UNBOUNDED='--unbounded'
PIPE_FLAG_OUT_RATE='--out-rate'
PIPE_FLAG_MANIFEST='--manifest'
PIPE_FLAG_METRICS='--metrics'
VERIFY_SCRIPT_DEFAULT='scripts/verify.sh'
SUMMARY_HEADER='bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s'
OCCUPANCY_HEADER='t_ms,q1_size,q2_size'

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
RESULTS_ROOT=$(realpath -m -- "$SCRIPT_DIR/results")
BIN_PATH=
INPUT_PATH=
OUT_DIR=
CACHE_REQUESTED=warm
DO_VERIFY=0
MEMORY_QUEUE=4
MEMORY_WORKERS=4
MEMORY_BLOCK_SIZE=65536
OUT_RATE_MB_S=10

usage() {
  cat >&2 <<'USAGE'
Usage: bench/sweep.sh --bin PATH --input PATH [--out-dir PATH] [--cache=warm|larger|drop] [--verify]
       [--memory-queue N] [--memory-workers N] [--memory-block-size BYTES] [--out-rate MB/s]
RUNS defaults to 5 and INPUT_SIZE to 1073741824 bytes. Memory output rate defaults to 10 decimal MB/s.
USAGE
}
error() { printf 'sweep: %s\n' "$*" >&2; }
die() { error "$*"; exit 1; }
is_positive_integer() { [[ ${1:-} =~ ^[0-9]+$ ]] && (( 10#$1 > 0 )); }
is_positive_rate() {
  awk -v value="$1" 'BEGIN { if (value ~ /^[0-9]+([.][0-9]+)?$/ && value+0 > 0 && value+0 <= 1000000000) exit 0; exit 1 }'
}
canonicalize_path_m() {
  local path=$1 probe=$1 suffix= leaf parent resolved
  while [[ ! -e "$probe" && ! -L "$probe" ]]; do
    leaf=${probe##*/}
    suffix="/$leaf$suffix"
    if [[ "$probe" == */* ]]; then parent=${probe%/*}; else parent=.; fi
    [[ -n "$parent" ]] || parent=/
    probe=$parent
  done
  resolved=$(realpath -m -- "$probe") || return 1
  realpath -m -- "$resolved$suffix"
}
parse_value() {
  local option=$1
  if (($# < 2)); then usage; die "missing value for $option"; fi
  printf '%s' "$2"
}

while (($#)); do
  case "$1" in
    --bin) BIN_PATH=$(parse_value "$1" "${2-}"); shift 2 ;;
    --bin=*) BIN_PATH=${1#*=}; shift ;;
    --input) INPUT_PATH=$(parse_value "$1" "${2-}"); shift 2 ;;
    --input=*) INPUT_PATH=${1#*=}; shift ;;
    --out-dir) OUT_DIR=$(parse_value "$1" "${2-}"); shift 2 ;;
    --out-dir=*) OUT_DIR=${1#*=}; shift ;;
    --cache) CACHE_REQUESTED=$(parse_value "$1" "${2-}"); shift 2 ;;
    --cache=*) CACHE_REQUESTED=${1#*=}; shift ;;
    --verify) DO_VERIFY=1; shift ;;
    --memory-queue) MEMORY_QUEUE=$(parse_value "$1" "${2-}"); shift 2 ;;
    --memory-queue=*) MEMORY_QUEUE=${1#*=}; shift ;;
    --memory-workers) MEMORY_WORKERS=$(parse_value "$1" "${2-}"); shift 2 ;;
    --memory-workers=*) MEMORY_WORKERS=${1#*=}; shift ;;
    --memory-block-size) MEMORY_BLOCK_SIZE=$(parse_value "$1" "${2-}"); shift 2 ;;
    --memory-block-size=*) MEMORY_BLOCK_SIZE=${1#*=}; shift ;;
    --out-rate) OUT_RATE_MB_S=$(parse_value "$1" "${2-}"); shift 2 ;;
    --out-rate=*) OUT_RATE_MB_S=${1#*=}; shift ;;
    -h|--help) usage >&1; exit 0 ;;
    *) usage; die "unknown argument: $1" ;;
  esac
done

[[ -n "$BIN_PATH" ]] || { usage; die '--bin is required'; }
[[ -n "$INPUT_PATH" ]] || { usage; die '--input is required'; }
case "$CACHE_REQUESTED" in warm|larger|drop) ;; *) die "invalid cache mode: $CACHE_REQUESTED" ;; esac
RUNS=${RUNS:-5}
INPUT_SIZE=${INPUT_SIZE:-1073741824}
is_positive_integer "$RUNS" || die 'RUNS must be a positive integer'
is_positive_integer "$INPUT_SIZE" || die 'INPUT_SIZE must be a positive integer'
is_positive_integer "$MEMORY_QUEUE" || die '--memory-queue must be a positive integer'
is_positive_integer "$MEMORY_WORKERS" || die '--memory-workers must be a positive integer'
is_positive_integer "$MEMORY_BLOCK_SIZE" || die '--memory-block-size must be a positive integer byte count'
is_positive_rate "$OUT_RATE_MB_S" || die '--out-rate must be a positive decimal rate no greater than 1000000000 MB/s'
RUNS=$((10#$RUNS))
INPUT_SIZE=$((10#$INPUT_SIZE))
MEMORY_QUEUE=$((10#$MEMORY_QUEUE))
MEMORY_WORKERS=$((10#$MEMORY_WORKERS))
MEMORY_BLOCK_SIZE=$((10#$MEMORY_BLOCK_SIZE))

if [[ ${PIPE_CHUNK_TESTING:-} != 1 ]]; then
  for seam in PIPE_CHUNK_PROC_VERSION_FILE PIPE_CHUNK_DROP_CACHES_FILE PIPE_CHUNK_VERIFY_SCRIPT SWEEP_TEST_UID; do
    [[ ! -v $seam ]] || die "$seam is available only with PIPE_CHUNK_TESTING=1"
  done
fi
if [[ -v SWEEP_TEST_UID ]]; then
  [[ ${PIPE_CHUNK_TESTING:-} == 1 && $SWEEP_TEST_UID =~ ^[0-9]+$ ]] || die 'invalid test uid override'
fi

BIN_PATH=$(realpath -m -- "$BIN_PATH")
INPUT_PATH=$(realpath -m -- "$INPUT_PATH")
[[ -f "$BIN_PATH" && -x "$BIN_PATH" ]] || die "binary is not executable: $BIN_PATH"
[[ -f "$INPUT_PATH" && -r "$INPUT_PATH" ]] || die "input is not a readable file: $INPUT_PATH"
source_bytes=$(stat -c '%s' -- "$INPUT_PATH") || die "cannot stat input: $INPUT_PATH"
(( source_bytes >= INPUT_SIZE )) || die "input is shorter than INPUT_SIZE ($source_bytes < $INPUT_SIZE bytes)"

if [[ -z "$OUT_DIR" ]]; then
  OUT_DIR="$RESULTS_ROOT/$(date -u +%Y%m%dT%H%M%SZ)"
elif [[ "$OUT_DIR" != /* ]]; then
  OUT_DIR="$REPO_ROOT/$OUT_DIR"
fi
OUT_DIR=$(canonicalize_path_m "$OUT_DIR") || die 'cannot canonicalize result path'
[[ "$OUT_DIR" == "$RESULTS_ROOT/"* ]] || die "result path must be strictly inside $RESULTS_ROOT: $OUT_DIR"
[[ ! -e "$OUT_DIR" && ! -L "$OUT_DIR" ]] || die "result path already exists: $OUT_DIR"

EFFECTIVE_CACHE=$CACHE_REQUESTED
PROC_VERSION_FILE=${PIPE_CHUNK_PROC_VERSION_FILE:-/proc/version}
DROP_CACHES_FILE=${PIPE_CHUNK_DROP_CACHES_FILE:-/proc/sys/vm/drop_caches}
VERIFY_SCRIPT=${PIPE_CHUNK_VERIFY_SCRIPT:-"$REPO_ROOT/$VERIFY_SCRIPT_DEFAULT"}
if [[ "$CACHE_REQUESTED" == drop ]]; then
  [[ -r "$PROC_VERSION_FILE" ]] || die "cannot read proc version file: $PROC_VERSION_FILE"
  if grep -qi microsoft "$PROC_VERSION_FILE" || [[ -v WSL_DISTRO_NAME ]]; then
    EFFECTIVE_CACHE=wsl-no-drop
  else
    caller_uid=$(id -u)
    if [[ -v SWEEP_TEST_UID ]]; then caller_uid=$SWEEP_TEST_UID; fi
    if (( caller_uid != 0 )); then
      sudo -n true >/dev/null 2>&1 || die 'cache drop requires root or successful passwordless sudo -n'
    fi
  fi
fi
if (( DO_VERIFY )); then [[ -x "$VERIFY_SCRIPT" ]] || die "verify script is not executable: $VERIFY_SCRIPT"; fi

mkdir -p -- "$(dirname -- "$OUT_DIR")" || die "cannot create result parent: $(dirname -- "$OUT_DIR")"
mkdir -- "$OUT_DIR" || die "cannot reserve fresh result directory: $OUT_DIR"
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/w3-01-sweep.XXXXXX") || die 'cannot create scratch directory'
cleanup() { [[ -z ${SCRATCH:-} ]] || rm -rf -- "$SCRATCH"; }
trap cleanup EXIT
EFFECTIVE_INPUT="$SCRATCH/input.prefix"
head -c "$INPUT_SIZE" -- "$INPUT_PATH" >"$EFFECTIVE_INPUT" || die 'cannot prepare effective input prefix'
[[ $(stat -c '%s' -- "$EFFECTIVE_INPUT") == "$INPUT_SIZE" ]] || die 'effective input prefix has the wrong size'

TRIALS_HEADER="$SUMMARY_HEADER,run_group,mode,status,queue_cap,workers,block_bytes,input_bytes,cache_method,trial,out_rate_mb_s"
MEDIANS_HEADER="$SUMMARY_HEADER,run_group,mode,queue_cap,workers,block_bytes,input_bytes,cache_method,out_rate_mb_s"
printf '%s\n' "$TRIALS_HEADER" >"$OUT_DIR/trials.csv"
printf '%s\n' "$MEDIANS_HEADER" >"$OUT_DIR/medians.csv"
printf '%s\n' "$TRIALS_HEADER" >"$OUT_DIR/memory_trials.csv"
printf '%s\n' "$MEDIANS_HEADER" >"$OUT_DIR/memory_medians.csv"

if [[ "$CACHE_REQUESTED" == larger ]]; then
  mem_total_kb=$(awk '$1=="MemTotal:" {print $2; exit}' /proc/meminfo)
  [[ "$mem_total_kb" =~ ^[0-9]+$ ]] || die 'cannot read MemTotal from /proc/meminfo'
  (( INPUT_SIZE > mem_total_kb * 1024 )) || die "--cache=larger requires INPUT_SIZE greater than MemTotal ($((mem_total_kb * 1024)) bytes)"
fi

cache_prepare() {
  [[ "$CACHE_REQUESTED" == drop && "$EFFECTIVE_CACHE" == drop ]] || return 0
  sync || { error 'sync failed before native cache drop'; return 1; }
  local caller_uid
  caller_uid=$(id -u)
  if [[ -v SWEEP_TEST_UID ]]; then caller_uid=$SWEEP_TEST_UID; fi
  if (( caller_uid == 0 )); then
    printf '3\n' >"$DROP_CACHES_FILE" || { error "cache drop write failed: $DROP_CACHES_FILE"; return 1; }
  else
    printf '3\n' | sudo -n tee "$DROP_CACHES_FILE" >/dev/null || { error "cache drop write failed: $DROP_CACHES_FILE"; return 1; }
  fi
}

METRIC_ROW=
occupancy_valid() {
  local path=$1 header
  [[ -r "$path" ]] || return 1
  IFS= read -r header <"$path" || return 1
  [[ "$header" == "$OCCUPANCY_HEADER" ]]
}
parse_summary() {
  local path=$1
  [[ -r "$path" ]] || return 1
  awk -F, -v wanted="$SUMMARY_HEADER" '
    BEGIN { n=split(wanted, names, ",") }
    NR==1 {
      if(NF!=n) { bad=1; next }
      for(i=1;i<=NF;i++) { if(col[$i]) bad=1; col[$i]=i }
      for(i=1;i<=n;i++) if(!col[names[i]]) bad=1
      next
    }
    NR==2 {
      if(NF!=n) { bad=1; next }
      for(i=1;i<=n;i++) {
        value=$(col[names[i]])
        if(value!="" && value!="n/a" && value !~ /^[-+]?[0-9]+([.][0-9]*)?([eE][-+]?[0-9]+)?$/) bad=1
        printf "%s%s", value, (i==n ? "\n" : ",")
      }
      rows++
      next
    }
    NR>2 { bad=1 }
    END { if(bad || rows!=1) exit 1 }
  ' "$path"
}

append_trial() {
  local file=$1 metrics=$2 group=$3 mode=$4 status=$5 queue=$6 workers=$7 block=$8 trial=$9 rate=${10}
  if [[ "$status" == success ]]; then
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$metrics" "$group" "$mode" "$status" "$queue" "$workers" "$block" "$INPUT_SIZE" "$EFFECTIVE_CACHE" "$trial" "$rate" >>"$file"
  else
    local empty_metrics= i
    for ((i=0; i<10; i++)); do empty_metrics+=','; done
    printf '%s%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$empty_metrics" "$group" "$mode" "$status" "$queue" "$workers" "$block" "$INPUT_SIZE" "$EFFECTIVE_CACHE" "$trial" "$rate" >>"$file"
  fi
}

run_one() {
  local group=$1 mode=$2 queue=$3 workers=$4 block=$5 trial=$6 rate=$7 prefix verify_this=0
  local output manifest status=success metric_row= summary
  local -a args=()
  case "$group" in
    core) prefix="$SCRATCH/metrics_core_q${queue}_w${workers}_b${block}_t$(printf '%02d' "$trial")" ;;
    baseline) prefix="$SCRATCH/metrics_baseline_b${block}_t$(printf '%02d' "$trial")" ;;
    *) die "internal unsupported run group: $group" ;;
  esac
  output="$SCRATCH/output_${prefix##*/}"
  manifest="$SCRATCH/manifest_${prefix##*/}.txt"
  if ! cache_prepare; then return 3; fi
  args+=("${PIPE_FLAG_MODE}=$mode")
  if [[ "$group" == core ]]; then
    args+=("${PIPE_FLAG_QUEUE}=$queue" "${PIPE_FLAG_WORKERS}=$workers" "${PIPE_FLAG_BLOCK_SIZE}=$block")
  else
    args+=("${PIPE_FLAG_BLOCK_SIZE}=$block")
  fi
  if [[ "$group" == core && $DO_VERIFY -eq 1 && $trial -eq 1 ]]; then
    verify_this=1
    args+=("${PIPE_FLAG_MANIFEST}=$manifest")
  fi
  args+=("${PIPE_FLAG_METRICS}=$prefix" "$EFFECTIVE_INPUT" "$output")
  if "$BIN_PATH" "${args[@]}"; then
    summary="$prefix.summary.csv"
    if metric_row=$(parse_summary "$summary"); then
      if (( verify_this )); then
        if ! "$VERIFY_SCRIPT" "$EFFECTIVE_INPUT" "$output" "$manifest"; then
          append_trial "$OUT_DIR/trials.csv" '' "$group" "$mode" failed "$queue" "$workers" "$block" "$trial" ''
          error "verification failed for $prefix; stopping immediately"
          return 2
        fi
      fi
      if [[ "$group" == core && $trial -eq 1 ]]; then
        if ! occupancy_valid "$prefix.occupancy.csv" || ! cp -- "$prefix.occupancy.csv" "$OUT_DIR/occ_q${queue}_w${workers}_b${block}_t1.csv"; then
          status=failed
        fi
      fi
    else
      status=failed
    fi
  else
    status=failed
  fi
  append_trial "$OUT_DIR/trials.csv" "$metric_row" "$group" "$mode" "$status" "$queue" "$workers" "$block" "$trial" ''
  [[ "$status" == success ]]
}

measure_reader_rate() {
  local started ended elapsed
  if ! cache_prepare; then return 1; fi
  started=$(date +%s%N) || return 1
  cat -- "$EFFECTIVE_INPUT" >/dev/null || return 1
  ended=$(date +%s%N) || return 1
  elapsed=$((ended - started))
  (( elapsed > 0 )) || elapsed=1
  awk -v bytes="$INPUT_SIZE" -v ns="$elapsed" 'BEGIN { printf "%.6f", bytes*1000/ns }'
}
duration_estimate() {
  awk -v runs="$RUNS" -v bytes="$INPUT_SIZE" -v rate="$OUT_RATE_MB_S" '
    BEGIN {
      seconds=int((2*runs*bytes)/(rate*1000000))
      if ((2*runs*bytes)/(rate*1000000) > seconds) seconds++
      printf "%02d:%02d:%02d", int(seconds/3600), int(seconds%3600/60), seconds%60
    }
  '
}
run_memory_one() {
  local mode=$1 trial=$2 prefix output status=success metric_row= summary queue=$MEMORY_QUEUE workers=$MEMORY_WORKERS block=$MEMORY_BLOCK_SIZE rate=$OUT_RATE_MB_S
  local -a args=()
  prefix="$SCRATCH/metrics_memory_${mode}_q${queue}_w${workers}_b${block}_t$(printf '%02d' "$trial")"
  output="$SCRATCH/output_${prefix##*/}"
  if ! cache_prepare; then return 3; fi
  args+=("${PIPE_FLAG_MODE}=pipeline" "${PIPE_FLAG_QUEUE}=$queue" "${PIPE_FLAG_WORKERS}=$workers" "${PIPE_FLAG_BLOCK_SIZE}=$block" "${PIPE_FLAG_OUT_RATE}=$rate")
  if [[ "$mode" == unbounded ]]; then args+=("$PIPE_FLAG_UNBOUNDED"); fi
  args+=("${PIPE_FLAG_METRICS}=$prefix" "$EFFECTIVE_INPUT" "$output")
  if "$BIN_PATH" "${args[@]}"; then
    summary="$prefix.summary.csv"
    if metric_row=$(parse_summary "$summary") && occupancy_valid "$prefix.occupancy.csv" && cp -- "$prefix.occupancy.csv" "$OUT_DIR/${prefix##*/}.occupancy.csv"; then
      status=success
    else
      status=failed
    fi
  else
    status=failed
  fi
  append_trial "$OUT_DIR/memory_trials.csv" "$metric_row" memory "$mode" "$status" "$queue" "$workers" "$block" "$trial" "$rate"
  [[ "$status" == success ]]
}

successful_count() {
  local file=$1 group=$2 mode=$3 queue=$4 workers=$5 block=$6
  awk -F, -v g="$group" -v m="$mode" -v q="$queue" -v w="$workers" -v b="$block" '
    NR==1 { for(i=1;i<=NF;i++) col[$i]=i; next }
    $(col["run_group"])==g && $(col["mode"])==m && $(col["queue_cap"])==q && $(col["workers"])==w && $(col["block_bytes"])==b && $(col["status"])=="success" { n++ }
    END { print n+0 }
  ' "$file"
}
metric_median() {
  local file=$1 group=$2 mode=$3 queue=$4 workers=$5 block=$6 metric=$7
  local values result
  values=$(awk -F, -v g="$group" -v m="$mode" -v q="$queue" -v w="$workers" -v b="$block" -v metric="$metric" '
    NR==1 { for(i=1;i<=NF;i++) col[$i]=i; next }
    $(col["run_group"])==g && $(col["mode"])==m && $(col["queue_cap"])==q && $(col["workers"])==w && $(col["block_bytes"])==b && $(col["status"])=="success" {
      v=$(col[metric]); rows++
      if(v=="") empty++
      else if(v=="n/a") unavailable++
      else print v
    }
    END { if(rows && empty==rows) print "__ALL_EMPTY__"; else if(empty || unavailable) print "__NA__" }
  ' "$file")
  if [[ "$values" == *'__ALL_EMPTY__'* ]]; then printf ''; return 0; fi
  if [[ "$values" == *'__NA__'* ]]; then printf 'n/a'; return 0; fi
  result=$(printf '%s\n' "$values" | sort -n | awk -v n="$(printf '%s\n' "$values" | awk 'NF{n++} END{print n+0}')" '
    NF { a[NR]=$1; count=NR }
    END { if(count==0) exit 1; if(count%2) printf "%.10g", a[(count+1)/2]; else printf "%.10g", (a[count/2]+a[count/2+1])/2 }
  ') || return 1
  printf '%s' "$result"
}
append_medians_for_group() {
  local file=$1 median_file=$2 group=$3 mode=$4 queue=$5 workers=$6 block=$7 incomplete_name=$8
  local count metric metrics= value first=1
  count=$(successful_count "$file" "$group" "$mode" "$queue" "$workers" "$block")
  if (( count < RUNS )); then
    INCOMPLETE_GROUPS=1
    return 0
  fi
  for metric in bytes blocks wall_s throughput_mb_s lat_p50_us lat_p99_us cpu_user_s cpu_sys_s peak_rss_kb io_call_s; do
    value=$(metric_median "$file" "$group" "$mode" "$queue" "$workers" "$block" "$metric") || die "cannot calculate median for $incomplete_name $metric"
    if (( first == 0 )); then metrics+=','; fi
    first=0
    metrics+="$value"
  done
  local out_rate=
  if [[ "$group" == memory ]]; then out_rate=$OUT_RATE_MB_S; fi
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$metrics" "$group" "$mode" "$queue" "$workers" "$block" "$INPUT_SIZE" "$EFFECTIVE_CACHE" "$out_rate" >>"$median_file"
}

QUEUE_VALUES=(1 4 16 64)
WORKER_VALUES=(1 2 4 8)
BLOCK_VALUES=(4096 65536 1048576)
INCOMPLETE_GROUPS=0
ABORT_NOW=0
MEMORY_CALIBRATED=0
MEMORY_RATE_OK=1
for ((trial=1; trial<=RUNS; trial++)); do
  for queue in "${QUEUE_VALUES[@]}"; do
    for workers in "${WORKER_VALUES[@]}"; do
      for block in "${BLOCK_VALUES[@]}"; do
        if run_one core pipeline "$queue" "$workers" "$block" "$trial" ''; then :; else rc=$?; if (( rc == 2 || rc == 3 )); then ABORT_NOW=1; break 3; fi; fi
      done
    done
  done
  (( ABORT_NOW == 0 )) || break
  for block in "${BLOCK_VALUES[@]}"; do
    if run_one baseline blocking '' 1 "$block" "$trial" ''; then :; else rc=$?; if (( rc == 2 || rc == 3 )); then ABORT_NOW=1; break; fi; fi
  done
  (( ABORT_NOW == 0 )) || break
  if (( trial == 1 && MEMORY_CALIBRATED == 0 )); then
    MEMORY_CALIBRATED=1
    if measured_rate=$(measure_reader_rate); then
      if ! awk -v configured="$OUT_RATE_MB_S" -v measured="$measured_rate" 'BEGIN { exit !(configured+0 < measured+0) }'; then
        error "--out-rate=$OUT_RATE_MB_S decimal MB/s must be below measured sequential read rate $measured_rate MB/s"
        MEMORY_RATE_OK=0
        INCOMPLETE_GROUPS=1
      else
        estimate=$(duration_estimate)
        printf 'Estimated memory series: %s at %s decimal MB/s, assuming output-bound runs; setup/teardown overhead excluded.\n' "$estimate" "$OUT_RATE_MB_S"
      fi
    else
      error 'could not measure sequential input read rate for memory series'
      MEMORY_RATE_OK=0
      INCOMPLETE_GROUPS=1
    fi
  fi
  if (( MEMORY_RATE_OK )); then
    for mode in bounded unbounded; do
      if run_memory_one "$mode" "$trial"; then :; else rc=$?; if (( rc == 3 )); then ABORT_NOW=1; break; fi; fi
    done
  fi
  (( ABORT_NOW == 0 )) || break
done

for queue in "${QUEUE_VALUES[@]}"; do
  for workers in "${WORKER_VALUES[@]}"; do
    for block in "${BLOCK_VALUES[@]}"; do
      append_medians_for_group "$OUT_DIR/trials.csv" "$OUT_DIR/medians.csv" core pipeline "$queue" "$workers" "$block" "run_group=core mode=pipeline queue_cap=$queue workers=$workers block_bytes=$block"
    done
  done
done
for block in "${BLOCK_VALUES[@]}"; do
  append_medians_for_group "$OUT_DIR/trials.csv" "$OUT_DIR/medians.csv" baseline blocking '' 1 "$block" "run_group=baseline mode=blocking block_bytes=$block"
done
for mode in bounded unbounded; do
  append_medians_for_group "$OUT_DIR/memory_trials.csv" "$OUT_DIR/memory_medians.csv" memory "$mode" "$MEMORY_QUEUE" "$MEMORY_WORKERS" "$MEMORY_BLOCK_SIZE" "run_group=memory mode=$mode queue_cap=$MEMORY_QUEUE workers=$MEMORY_WORKERS block_bytes=$MEMORY_BLOCK_SIZE"
done

if (( ABORT_NOW || INCOMPLETE_GROUPS )); then
  error "sweep incomplete; results preserved in $OUT_DIR"
  exit 1
fi
printf '%s\n' "$OUT_DIR"
