# W3-01 Result Schema Contract

This contract preserves the W2-06 measured headers exactly and defines the file names and metadata consumed by W3-02.

## Producer source columns

src/metrics.c writes one header and one data row to PREFIX.summary.csv:

bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s

It writes samples to PREFIX.occupancy.csv with this exact header:

t_ms,q1_size,q2_size

Consumers must use these names unchanged. Summary values are decimal text. Latency cells can be empty when there were no latency samples or n/a when samples were dropped; unavailable resource and I/O fields can be n/a.

## Result files

All files are written in the fresh result directory.

### trials.csv

Contains one row for each attempted core or blocking-baseline invocation, including failed attempts. Exact ordered header:

bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s,run_group,mode,status,queue_cap,workers,block_bytes,input_bytes,cache_method,trial,out_rate_mb_s

run_group is core or baseline. mode is pipeline or blocking. status is success or failed. queue_cap is populated for core and empty for baseline. workers is 1, 2, 4, or 8 for core and 1 for baseline. block_bytes is a byte count. input_bytes is the effective input prefix length. cache_method is warm, larger, drop, or wsl-no-drop. trial is 1..RUNS. out_rate_mb_s is empty outside memory runs. Failed rows keep the known metadata and have empty measured fields.

### medians.csv

Contains one row for each complete core or baseline configuration group and no rows for incomplete groups. Exact ordered header:

bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s,run_group,mode,queue_cap,workers,block_bytes,input_bytes,cache_method,out_rate_mb_s

Rows use the same group/configuration values as trials.csv except status and trial. Each numeric metric median is calculated from RUNS successful values using sort -n. For odd RUNS, select the center value; for even RUNS, average the two center values. If a metric is unavailable in any successful row, its median cell is n/a (or empty only when the producer cell is empty for all rows); downstream plots that require a numeric value must fail clearly.

### memory_trials.csv

Contains one row per bounded or unbounded memory invocation, including failed attempts. It uses the exact trials.csv header. run_group is memory; mode is bounded or unbounded. Both modes carry the same positive queue_cap, workers, block_bytes, input_bytes, cache_method, and out_rate_mb_s for a matched comparison. Pipeline flags match except the unbounded invocation adds --unbounded.

### memory_medians.csv

Contains one row per complete bounded or unbounded memory group and uses the exact medians.csv header. A memory point is keyed by run_group, mode, queue_cap, workers, block_bytes, input_bytes, cache_method, and out_rate_mb_s.

## Occupancy artifacts retained for consumers

- Core: keep trial 1 only as occ_q<Q>_w<W>_b<B>_t1.csv, such as occ_q16_w4_b65536_t1.csv. The source file is that invocation's PREFIX.occupancy.csv.
- Memory: keep every memory invocation's exact PREFIX.occupancy.csv. Use unique prefixes containing run group, mode, queue capacity, worker count, block size, and trial; recommended basename format is metrics_memory_<mode>_q<Q>_w<W>_b<B>_t<TRIAL>.occupancy.csv.
- Other core and baseline occupancy files may be removed after aggregation.

Occupancy rows contain milliseconds from run origin and Q1/Q2 sizes. A memory occupancy file is associated with its mode and configuration through its prefix.

## Group and empty-field rules

- Valid group/mode pairs are core/pipeline, baseline/blocking, and memory/bounded or memory/unbounded.
- queue_cap is empty only for baseline rows. It is positive and identical for bounded and unbounded memory rows.
- out_rate_mb_s is empty for core and baseline, and positive for both memory modes.
- cache_method and input_bytes are present in every raw trial row.
- Warm-cache results are not disk-effect measurements.
- medians.csv and memory_medians.csv contain complete groups only. Every incomplete group has no median; the sweep exits nonzero if any group is incomplete.
- Summary column order and spelling are producer-owned and must not change in these result files.