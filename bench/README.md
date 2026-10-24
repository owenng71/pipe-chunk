# Benchmark result contract (W3-01)

`sweep.sh` writes a fresh result directory under `bench/results/`. This file defines the stable
CSV schema owned by W3-01; W3-02's plotting tools consume it and must not redefine it. Result
directories are ignored by Git.

Warm-cache measurements characterize the pipeline with the input already subject to normal page
cache behavior. They are **not disk-effect measurements**. Use the `larger` or `drop` cache modes
only when the stated prerequisites are satisfied.

## `runs.csv`

The first row is this exact header:

```text
run_group,mode,status,queue_cap,workers,block_bytes,trial,input_bytes,cache_method,out_rate_mb_s,bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s
```

Each following row is one attempted pipeline invocation. Failed attempts remain in the file.
`input_bytes` and `cache_method` are known before an invocation and are present on successful and
failed rows alike. `status` is `success` only when cache preparation, the command, its W2-06 summary,
and any required trial-1 verification succeed; otherwise it is `failed`. When a metrics summary exists and can be
parsed, available metrics may remain on a failed row; unavailable metric values are empty.

| Field | Meaning and empty-field rule |
|---|---|
| `run_group` | `core`, `baseline`, or `memory`. |
| `mode` | `pipeline` for core; `blocking` for baseline; `bounded` or `unbounded` for memory. |
| `status` | `success` or `failed`. |
| `queue_cap` | Q1/Q2 capacity for core and memory. Empty for baseline. Bounded and unbounded memory rows record the same configured positive capacity. |
| `workers` | Worker count; baseline records `1` because it is a one-thread mode. |
| `block_bytes` | Block size in bytes. |
| `trial` | Trial number, 1 through 5. |
| `input_bytes` | Input file size in bytes, including on failed rows. |
| `cache_method` | `warm`, `larger`, or `drop`, including on failed rows. |
| `out_rate_mb_s` | Positive output throttle in MB/s for memory rows; empty for core and baseline. |
| `bytes`, `blocks` | W2-06 total bytes and completed blocks. |
| `wall_s`, `throughput_mb_s` | W2-06 wall time and throughput. |
| `lat_p50_us`, `lat_p99_us` | W2-06 latency percentiles in microseconds. |
| `cpu_user_s`, `cpu_sys_s` | User and system CPU time from W2-06, in seconds. |
| `peak_rss_kb` | W2-06 peak resident memory in KiB. |
| `io_call_s` | W2-06 process I/O-call elapsed time in seconds. |

Metric columns are empty when unavailable. A missing or malformed `PREFIX.summary.csv` makes that
trial fail; it never gets silently treated as a successful zero-valued result.

## `medians.csv`

The first row is this exact header:

```text
run_group,mode,queue_cap,workers,block_bytes,input_bytes,cache_method,out_rate_mb_s,bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s
```

There is one row per complete configuration group, containing the median of each available scalar
metric over five successful trials. A group is keyed by `run_group`, `mode`, `queue_cap`, `workers`,
`block_bytes`, `input_bytes`, `cache_method`, and `out_rate_mb_s`. Groups with fewer than five
successful trials have no median row. `status` and `trial` are omitted because a median row
summarizes a complete group rather than one attempt.

For both CSVs, valid group/mode pairs are:

| `run_group` | `mode` | `queue_cap` |
|---|---|---|
| `core` | `pipeline` | One of 1, 4, 16, or 64. |
| `baseline` | `blocking` | Empty. |
| `memory` | `bounded` or `unbounded` | The same configured capacity for both modes. |

The memory comparison uses the same input, queue capacity, workers, block size, cache method, and
positive `out_rate_mb_s` in both modes. Its pipeline flags are identical except that the unbounded
run adds `--unbounded`.

## Occupancy artifacts

The sweep retains the W2-06 occupancy CSV for trial 1 of each successful core configuration as
`occ_q<Q>_w<W>_b<B>_t1.csv`. The occupancy columns remain `t_ms,q1_size,q2_size`; other per-run
occupancy files are scratch and are removed when the sweep exits.

W2-06 metric names and units are the source for the scalar columns above. If an input set or metric
name changes, update this contract and W3-02 together through the normal team review.
