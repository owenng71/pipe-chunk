#!/usr/bin/env python3
"""plot.py [RESULTS] — graphs from bench/sweep.sh and bench/memory.sh output.

RESULTS defaults to bench/results. Writes PNGs to RESULTS/plots/:
  throughput_vs_workers.png  median throughput per worker count, one panel per block size,
                             one line per queue size; dashed line = blocking baseline;
                             bars = min..max over the runs
  latency_vs_queue.png       p50 latency per queue size (line) up to p99 (band), one line per
                             worker count, one panel per block size
  occupancy.png              Q1 and Q2 sizes over time at queue 16, 64K blocks, for 1, 2, 4, 8
                             workers: shows which stage is the bottleneck
  memory.png                 peak RSS vs file size with a slow writer, bounded vs --unbounded,
                             and buffered blocks over time for the largest file
Needs matplotlib only (Ubuntu: python3-matplotlib).
"""
import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")  # files only, no display needed
import matplotlib.pyplot as plt  # noqa: E402

BLOCK_LABELS = {4096: "4 KiB", 65536: "64 KiB", 1048576: "1 MiB"}
MB = 1e6  # throughput is MB/s (10^6), as printed by pipe-chunk


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def num(s):
    return float("nan") if s in ("", "n/a") else float(s)


def block_label(b):
    return BLOCK_LABELS.get(b, f"{b} B")


def save(fig, out, name):
    path = os.path.join(out, name)
    fig.savefig(path, dpi=130, bbox_inches="tight")
    plt.close(fig)
    print(f"plot: {path}")


def sweep_rows(results):
    rows = read_csv(os.path.join(results, "sweep.csv"))
    pipe = [r for r in rows if r["mode"] == "pipeline"]
    base = {int(r["block"]): num(r["throughput_mb_s"]) for r in rows if r["mode"] == "blocking"}
    blocks = sorted({int(r["block"]) for r in pipe})
    queues = sorted({int(r["queue"]) for r in pipe})
    workers = sorted({int(r["workers"]) for r in pipe})
    return pipe, base, blocks, queues, workers


def pick(rows, **want):
    return [r for r in rows if all(int(r[k]) == v for k, v in want.items())]


def plot_throughput(results, out):
    pipe, base, blocks, queues, workers = sweep_rows(results)
    fig, axes = plt.subplots(1, len(blocks), figsize=(4.2 * len(blocks), 3.8),
                             sharey=True, constrained_layout=True)
    axes = axes if len(blocks) > 1 else [axes]
    for ax, b in zip(axes, blocks):
        for q in queues:
            rs = sorted(pick(pipe, block=b, queue=q), key=lambda r: int(r["workers"]))
            x = [int(r["workers"]) for r in rs]
            y = [num(r["throughput_mb_s"]) / 1000 for r in rs]
            lo = [yi - num(r["throughput_min_mb_s"]) / 1000 for yi, r in zip(y, rs)]
            hi = [num(r["throughput_max_mb_s"]) / 1000 - yi for yi, r in zip(y, rs)]
            ax.errorbar(x, y, yerr=[lo, hi], marker="o", capsize=3, label=f"queue {q}")
        if b in base:
            ax.axhline(base[b] / 1000, color="gray", linestyle="--", label="blocking baseline")
        ax.set_title(f"block {block_label(b)}")
        ax.set_xlabel("workers")
        ax.set_xscale("log", base=2)
        ax.set_xticks(workers, [str(w) for w in workers])
        ax.grid(alpha=0.3)
    top = max(num(r["throughput_max_mb_s"]) for r in pipe) / 1000
    axes[0].set_ylim(0, top * 1.08)
    axes[0].set_ylabel("throughput (GB/s, median)")
    axes[-1].legend(fontsize=8, loc="lower right")
    fig.suptitle("Throughput vs worker count")
    save(fig, out, "throughput_vs_workers.png")


def plot_latency(results, out):
    pipe, _, blocks, queues, workers = sweep_rows(results)
    fig, axes = plt.subplots(1, len(blocks), figsize=(4.2 * len(blocks), 3.8),
                             constrained_layout=True)
    axes = axes if len(blocks) > 1 else [axes]
    for ax, b in zip(axes, blocks):
        for w in workers:
            rs = sorted(pick(pipe, block=b, workers=w), key=lambda r: int(r["queue"]))
            x = [int(r["queue"]) for r in rs]
            p50 = [num(r["lat_p50_us"]) / 1000 for r in rs]
            p99 = [num(r["lat_p99_us"]) / 1000 for r in rs]
            line, = ax.plot(x, p50, marker="o", label=f"{w} worker{'s' if w > 1 else ''}")
            ax.fill_between(x, p50, p99, color=line.get_color(), alpha=0.12)
        ax.set_title(f"block {block_label(b)}")
        ax.set_xlabel("queue capacity (Q1 and Q2)")
        ax.set_xscale("log", base=2)
        ax.set_xticks(queues, [str(q) for q in queues])
        ax.set_yscale("log")
        ax.grid(alpha=0.3, which="both")
    axes[0].set_ylabel("block latency (ms): p50 line, band to p99")
    axes[0].legend(fontsize=8, loc="upper left")
    fig.suptitle("Latency vs queue size (read → written)")
    save(fig, out, "latency_vs_queue.png")


def plot_occupancy(results, out, queue=16, block="64K"):
    occ = os.path.join(results, "occupancy")
    found = []
    for w in (1, 2, 4, 8):
        path = os.path.join(occ, f"pipeline_q{queue}_w{w}_b{block}.csv")
        if os.path.exists(path):
            found.append((w, read_csv(path)))
    if not found:
        print(f"plot: no occupancy files for queue {queue}, block {block}; skipped occupancy.png")
        return
    fig, axes = plt.subplots(1, len(found), figsize=(3.6 * len(found), 3.4), sharey=True,
                             constrained_layout=True)
    axes = axes if len(found) > 1 else [axes]
    for ax, (w, rows) in zip(axes, found):
        t = [int(r["t_ms"]) for r in rows]
        ax.step(t, [int(r["q1_size"]) for r in rows], where="post", label="Q1 (reader → workers)")
        ax.step(t, [int(r["q2_size"]) for r in rows], where="post", label="Q2 (workers → writer)")
        ax.set_title(f"{w} worker{'s' if w > 1 else ''}")
        ax.set_xlabel("time (ms)")
        ax.set_ylim(-0.5, queue + 1)
        ax.grid(alpha=0.3)
    axes[0].set_ylabel(f"blocks in queue (capacity {queue})")
    axes[0].legend(fontsize=8, loc="center right")
    fig.suptitle(f"Queue occupancy over time, queue {queue}, block {block}, sampled every 10 ms")
    save(fig, out, "occupancy.png")


def plot_memory(results, out):
    path = os.path.join(results, "memory.csv")
    if not os.path.exists(path):
        print("plot: no memory.csv (run bench/memory.sh); skipped memory.png")
        return
    rows = read_csv(path)
    fig, (left, right) = plt.subplots(1, 2, figsize=(10, 3.8), constrained_layout=True)
    for mode, style in (("bounded", "o-"), ("unbounded", "s-")):
        rs = sorted((r for r in rows if r["mode"] == mode), key=lambda r: int(r["size"]))
        left.plot([int(r["size"]) / 2**20 for r in rs], [num(r["peak_rss_kb"]) / 1024 for r in rs],
                  style, label=mode)
    left.set_xlabel("file size (MiB)")
    left.set_ylabel("peak RSS (MiB)")
    left.set_title("Peak memory with a slow writer")
    left.grid(alpha=0.3)
    left.legend()
    for mode in ("bounded", "unbounded"):
        p = os.path.join(results, "occupancy", f"memory_{mode}.csv")
        if os.path.exists(p):
            occ = read_csv(p)
            right.step([int(r["t_ms"]) / 1000 for r in occ],
                       [int(r["q1_size"]) + int(r["q2_size"]) for r in occ], where="post", label=mode)
    right.set_yscale("symlog", linthresh=10)
    right.set_xlabel("time (s)")
    right.set_ylabel("blocks waiting in Q1 + Q2")
    right.set_title("Buffered blocks over time, largest file")
    right.grid(alpha=0.3, which="both")
    right.legend()
    fig.suptitle("Backpressure: bounded queues vs --unbounded (q 4, w 4, 64 KiB blocks)")
    save(fig, out, "memory.png")


def main():
    results = sys.argv[1] if len(sys.argv) > 1 else "bench/results"
    if not os.path.exists(os.path.join(results, "sweep.csv")):
        sys.exit(f"plot: {results}/sweep.csv not found, run bench/sweep.sh first")
    out = os.path.join(results, "plots")
    os.makedirs(out, exist_ok=True)
    plot_throughput(results, out)
    plot_latency(results, out)
    plot_occupancy(results, out)
    plot_memory(results, out)


if __name__ == "__main__":
    main()
