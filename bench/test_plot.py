#!/usr/bin/env python3
"""Temporary-fixture tests for the W3-02 benchmark plotter."""

import csv
import contextlib
import io
import itertools
import math
import re
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
PLOT = ROOT / "bench" / "plot.py"
RUNS_HEADER = (
    "run_group,mode,status,queue_cap,workers,block_bytes,trial,input_bytes,cache_method,"
    "out_rate_mb_s,bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,"
    "cpu_user_s,cpu_sys_s,peak_rss_kb,io_call_s"
).split(",")
MEDIANS_HEADER = (
    "run_group,mode,queue_cap,workers,block_bytes,input_bytes,cache_method,out_rate_mb_s,"
    "bytes,blocks,wall_s,throughput_mb_s,lat_p50_us,lat_p99_us,cpu_user_s,cpu_sys_s,"
    "peak_rss_kb,io_call_s"
).split(",")
OCCUPANCY_HEADER = ["t_ms", "q1_size", "q2_size"]
OUTPUT_NAMES = {
    "throughput_vs_workers.svg",
    "latency_vs_queue.svg",
    "occupancy_over_time.svg",
    "memory_bounded_vs_unbounded.svg",
}


def median_rows():
    """Build a complete W3-01 median set with deterministic, inspectable values."""
    rows = []
    for cap, workers, block in itertools.product(
        (1, 4, 16, 64), (1, 2, 4, 8), (4096, 65536, 1048576)
    ):
        rows.append({
            "run_group": "core", "mode": "pipeline", "queue_cap": cap,
            "workers": workers, "block_bytes": block, "input_bytes": 4096,
            "cache_method": "warm", "out_rate_mb_s": "",
            "bytes": 4096, "blocks": 8, "wall_s": "2", "throughput_mb_s":
                str(cap + workers + block // 4096),
            "lat_p50_us": str(cap + workers), "lat_p99_us": str(cap + workers + 5),
            "cpu_user_s": "0.5", "cpu_sys_s": "0.25", "peak_rss_kb": "2048",
            "io_call_s": "0.1",
        })
    for block in (4096, 65536, 1048576):
        rows.append({
            "run_group": "baseline", "mode": "blocking", "queue_cap": "",
            "workers": 1, "block_bytes": block, "input_bytes": 4096,
            "cache_method": "warm", "out_rate_mb_s": "", "bytes": 4096,
            "blocks": 8, "wall_s": "4", "throughput_mb_s": str(block // 4096),
            "lat_p50_us": "10", "lat_p99_us": "20", "cpu_user_s": "1",
            "cpu_sys_s": "0.2", "peak_rss_kb": "3072", "io_call_s": "0.2",
        })
    for mode, cap, rss in (("bounded", 4, 8192), ("unbounded", 4, 12288)):
        rows.append({
            "run_group": "memory", "mode": mode, "queue_cap": cap,
            "workers": 4, "block_bytes": 65536, "input_bytes": 1048576,
            "cache_method": "warm", "out_rate_mb_s": "10", "bytes": 1048576,
            "blocks": 16, "wall_s": "3", "throughput_mb_s": "12",
            "lat_p50_us": "30", "lat_p99_us": "60", "cpu_user_s": "1",
            "cpu_sys_s": "0.3", "peak_rss_kb": rss, "io_call_s": "0.4",
        })
    return rows


def write_csv(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=header, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def make_result_fixture(root, *, medians=None, runs=None, occupancy_name="occ_q16_w4_b65536_t1.csv",
                        occupancy_rows=None, occupancy_header=None):
    """Create a W3-01-shaped result directory entirely under a caller-owned temp root."""
    result_dir = root / "results"
    medians = median_rows() if medians is None else medians
    if runs is None:
        runs = []
        for index, row in enumerate(medians, 1):
            runs.append({**row, "status": "success", "trial": index % 5 + 1})
    write_csv(result_dir / "runs.csv", RUNS_HEADER, runs)
    write_csv(result_dir / "medians.csv", MEDIANS_HEADER, medians)
    occupancy_header = OCCUPANCY_HEADER if occupancy_header is None else occupancy_header
    occupancy_rows = ([{"t_ms": 0, "q1_size": 0, "q2_size": 1},
                       {"t_ms": 10, "q1_size": 2, "q2_size": 3},
                       {"t_ms": 20, "q1_size": 1, "q2_size": 0}]
                      if occupancy_rows is None else occupancy_rows)
    write_csv(result_dir / occupancy_name, occupancy_header, occupancy_rows)
    return result_dir, result_dir / occupancy_name


class PlotFixtureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.results, self.occupancy = make_result_fixture(self.root)

    def tearDown(self):
        self.temp.cleanup()

    def cli(self, *, results=None, occupancy=None, out=None, extra=()):
        args = [sys.executable, str(PLOT)]
        if results is not None:
            args += ["--results", str(results)]
        if occupancy is not None:
            args += ["--occupancy", str(occupancy)]
        if out is not None:
            args += ["--out", str(out)]
        args += list(extra)
        return subprocess.run(args, text=True, capture_output=True, cwd=ROOT)

    def assert_diagnostic(self, result, *needles):
        self.assertEqual(result.returncode, 1, result.stderr)
        lines = result.stderr.splitlines()
        self.assertEqual(len(lines), 1, result.stderr)
        self.assertTrue(lines[0].startswith("plot: "), lines[0])
        for needle in needles:
            self.assertIn(needle, lines[0])

    @staticmethod
    def svg(path):
        return ET.parse(path).getroot()

    @staticmethod
    def series(root):
        return root.findall(f".//{{http://www.w3.org/2000/svg}}g[@class='series']")

    def test_complete_fixture_publishes_four_inspectable_svgs(self):
        out = self.root / "published"
        result = self.cli(results=self.results, occupancy=self.occupancy, out=out)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(out))
        self.assertEqual(result.stderr, "")
        self.assertEqual({path.name for path in out.iterdir()}, OUTPUT_NAMES)

        expected_counts = {
            "throughput_vs_workers.svg": 6,
            "latency_vs_queue.svg": 8,
            "occupancy_over_time.svg": 2,
            "memory_bounded_vs_unbounded.svg": 2,
        }
        roots = {}
        for name, count in expected_counts.items():
            root = self.svg(out / name)
            roots[name] = root
            series = self.series(root)
            self.assertEqual(len(series), count)
            for item in series:
                self.assertTrue(item.get("data-series"))
                self.assertTrue(item.get("data-marker"))
                self.assertTrue(item.get("data-dash"))
                if name == "memory_bounded_vs_unbounded.svg":
                    self.assertTrue(item.get("fill"))
                else:
                    self.assertTrue(item.get("stroke"))
            for node in root.iter():
                if node.tag.endswith("text"):
                    self.assertIn(node.get("data-role"), {"title", "x-label", "y-label", "legend"})

        throughput = roots["throughput_vs_workers.svg"]
        core = next(item for item in self.series(throughput)
                    if item.get("data-series") == "pipeline, block=65536 B")
        core_points = [node for node in core if node.get("data-x") is not None]
        self.assertEqual([float(node.get("data-x")) for node in core_points], [1, 2, 4, 8])
        self.assertEqual([float(node.get("data-y")) for node in core_points], [33, 34, 36, 40])
        baseline = next(item for item in self.series(throughput)
                        if item.get("data-series") == "blocking baseline, block=65536 B")
        self.assertEqual(baseline.get("stroke"), core.get("stroke"))
        baseline_path = baseline.find("{http://www.w3.org/2000/svg}path")
        self.assertIn(" H ", baseline_path.get("d"))
        self.assertEqual(baseline_path.get("stroke-dasharray"), "8,5")
        move_match = re.search(r"M ([0-9.]+) ([0-9.]+) H ([0-9.]+)", baseline_path.get("d"))
        self.assertIsNotNone(move_match)
        self.assertEqual((float(move_match.group(1)), float(move_match.group(3))), (95.0, 850.0))
        baseline_point = next(node for node in baseline if node.get("data-x") is not None)
        self.assertEqual(float(baseline_point.get("data-y")), 16)
        self.assertTrue(any(node.text == "blocking baseline, block=65536 B"
                            for node in throughput.iter() if node.tag.endswith("text")))
        for item in self.series(throughput):
            self.assertEqual(len([node for node in item if node.get("data-x") is not None]),
                             1 if "baseline" in item.get("data-series") else 4)

        latency = roots["latency_vs_queue.svg"]
        p50 = next(item for item in self.series(latency)
                   if item.get("data-series") == "workers=4 p50")
        p50_points = [node for node in p50 if node.get("data-x") is not None]
        self.assertEqual([float(node.get("data-x")) for node in p50_points], [1, 4, 16, 64])
        self.assertEqual([float(node.get("data-y")) for node in p50_points], [5, 8, 20, 68])
        for item in self.series(latency):
            self.assertEqual(len([node for node in item if node.get("data-x") is not None]), 4)

        occupancy = roots["occupancy_over_time.svg"]
        q1 = next(item for item in self.series(occupancy) if item.get("data-series") == "Q1 occupancy")
        self.assertEqual([(node.get("data-x"), node.get("data-y")) for node in q1
                          if node.get("data-x") is not None], [("0.0", "0.0"), ("10.0", "2.0"), ("20.0", "1.0")])
        q2 = next(item for item in self.series(occupancy) if item.get("data-series") == "Q2 occupancy")
        self.assertEqual(len([node for node in q2 if node.get("data-x") is not None]), 3)
        title_text = [node.text for node in occupancy.iter()
                      if node.tag.endswith("text") and node.get("data-role") == "title"]
        self.assertIn("queue=16", title_text[0])
        self.assertIn("workers=4", title_text[0])
        self.assertIn("block=65536", title_text[0])

        memory = roots["memory_bounded_vs_unbounded.svg"]
        bars = self.series(memory)
        self.assertEqual({item.get("data-series") for item in bars}, {"bounded", "unbounded"})
        self.assertEqual(len({item.get("fill") for item in bars}), 2)
        bounded_bar = bars[0].find("{http://www.w3.org/2000/svg}rect")
        self.assertAlmostEqual(float(bounded_bar.get("data-value")), 8.388608)
        self.assertEqual(bounded_bar.get("data-x"), "bounded")
        self.assertAlmostEqual(float(bounded_bar.get("data-y")), 8.388608)
        self.assertTrue(any(text.text == "bounded" for text in memory.iter()
                            if text.tag.endswith("text")))
        self.assertTrue(any(text.text == "8.389 MB" for text in memory.iter()
                            if text.tag.endswith("text")))

        for name in ("throughput_vs_workers.svg", "latency_vs_queue.svg", "occupancy_over_time.svg"):
            tuples = [(node.get("stroke"), node.get("data-marker"), node.get("data-dash"))
                      for node in self.series(roots[name])]
            self.assertEqual(len(tuples), len(set(tuples)), name)
        for name in expected_counts:
            texts = [node.text or "" for node in roots[name].iter() if node.tag.endswith("text")]
            self.assertTrue(any("(MB/s)" in text or "(µs)" in text or "(records)" in text
                                or "(decimal MB)" in text for text in texts), name)

    def test_axis_helpers_handle_zero_and_equal_values(self):
        import plot as plot_module
        self.assertEqual(plot_module._numeric_range([0, 0], include_zero=True, nonnegative=True), (0.0, 1.0))
        low, high = plot_module._numeric_range([5, 5])
        self.assertLess(low, 5)
        self.assertGreater(high, 5)

        write_csv(self.occupancy, OCCUPANCY_HEADER, [
            {"t_ms": 3, "q1_size": 0, "q2_size": 0},
            {"t_ms": 3, "q1_size": 0, "q2_size": 0},
        ])
        out = self.root / "degenerate"
        result = self.cli(results=self.results, occupancy=self.occupancy, out=out)
        self.assertEqual(result.returncode, 0, result.stderr)
        svg = (out / "occupancy_over_time.svg").read_text(encoding="utf-8")
        self.assertNotRegex(svg, r"\b(?:NaN|Infinity|-Infinity)\b")
        root = self.svg(out / "occupancy_over_time.svg")
        for item in self.series(root):
            for point in item:
                for coord in ("cx", "cy", "x", "y"):
                    if point.get(coord) is not None:
                        self.assertTrue(math.isfinite(float(point.get(coord))))

    def test_missing_runs_and_medians_files_are_diagnostic(self):
        (self.results / "runs.csv").unlink()
        result = self.cli(results=self.results, occupancy=self.occupancy)
        self.assert_diagnostic(result, "runs.csv")
        write_csv(self.results / "runs.csv", RUNS_HEADER, [])
        (self.results / "medians.csv").unlink()
        result = self.cli(results=self.results, occupancy=self.occupancy)
        self.assert_diagnostic(result, "medians.csv")

    def test_missing_columns_and_malformed_required_values_are_diagnostic(self):
        (self.results / "medians.csv").write_text(
            ",".join(field for field in MEDIANS_HEADER if field != "throughput_mb_s") + "\n",
            encoding="utf-8")
        result = self.cli(results=self.results, occupancy=self.occupancy)
        self.assert_diagnostic(result, "medians.csv", "throughput_mb_s")

        write_csv(self.results / "medians.csv", MEDIANS_HEADER, median_rows())
        rows = median_rows()
        next(row for row in rows if row["run_group"] == "core" and row["queue_cap"] == 16
             and row["workers"] == 1 and row["block_bytes"] == 4096)["throughput_mb_s"] = "NaN"
        write_csv(self.results / "medians.csv", MEDIANS_HEADER, rows)
        result = self.cli(results=self.results, occupancy=self.occupancy)
        self.assert_diagnostic(result, "medians.csv", "throughput_mb_s")

        rows = median_rows()
        next(row for row in rows if row["run_group"] == "core" and row["queue_cap"] == 16
             and row["workers"] == 1 and row["block_bytes"] == 4096)["throughput_mb_s"] = ""
        write_csv(self.results / "medians.csv", MEDIANS_HEADER, rows)
        result = self.cli(results=self.results, occupancy=self.occupancy)
        self.assert_diagnostic(result, "medians.csv", "throughput_mb_s")

    def test_missing_and_duplicate_required_groups_are_diagnostic(self):
        rows = [row for row in median_rows() if not (
            row["run_group"] == "core" and row["queue_cap"] == 16 and
            row["workers"] == 4 and row["block_bytes"] == 65536)]
        write_csv(self.results / "medians.csv", MEDIANS_HEADER, rows)
        result = self.cli(results=self.results, occupancy=self.occupancy)
        self.assert_diagnostic(result, "medians.csv", "run_group=core", "queue_cap=16",
                               "workers=4", "block_bytes=65536")

        rows = median_rows()
        rows.append(dict(rows[0]))
        write_csv(self.results / "medians.csv", MEDIANS_HEADER, rows)
        result = self.cli(results=self.results, occupancy=self.occupancy)
        self.assert_diagnostic(result, "medians.csv", "duplicate")

    def test_memory_pair_rejects_each_controlled_field_mismatch(self):
        changes = {
            "input_bytes": 2097152,
            "workers": 2,
            "block_bytes": 4096,
            "cache_method": "larger",
            "out_rate_mb_s": "20",
        }
        for field, value in changes.items():
            with self.subTest(field=field):
                rows = median_rows()
                target = next(row for row in rows if row["run_group"] == "memory" and row["mode"] == "unbounded")
                target[field] = value
                write_csv(self.results / "medians.csv", MEDIANS_HEADER, rows)
                result = self.cli(results=self.results, occupancy=self.occupancy)
                self.assert_diagnostic(result, "medians.csv", field)

    def test_occupancy_name_header_values_order_and_sample_count(self):
        wrong_name = self.results / "bad-name.csv"
        write_csv(wrong_name, OCCUPANCY_HEADER, [{"t_ms": 0, "q1_size": 0, "q2_size": 0},
                                                 {"t_ms": 1, "q1_size": 1, "q2_size": 1}])
        result = self.cli(results=self.results, occupancy=wrong_name)
        self.assert_diagnostic(result, "bad-name.csv", "filename")

        wrong_header = self.results / "occ_q16_w4_b65536_t1.csv"
        write_csv(wrong_header, ["t_ms", "q1_size", "other"], [
            {"t_ms": 0, "q1_size": 0, "other": 1}, {"t_ms": 1, "q1_size": 1, "other": 2}])
        result = self.cli(results=self.results, occupancy=wrong_header)
        self.assert_diagnostic(result, wrong_header.name, "q2_size")

        write_csv(wrong_header, OCCUPANCY_HEADER, [
            {"t_ms": 5, "q1_size": 0, "q2_size": 0}, {"t_ms": 4, "q1_size": 1, "q2_size": 1}])
        result = self.cli(results=self.results, occupancy=wrong_header)
        self.assert_diagnostic(result, wrong_header.name, "t_ms")

        write_csv(wrong_header, OCCUPANCY_HEADER, [{"t_ms": 0, "q1_size": "bad", "q2_size": 1},
                                                   {"t_ms": 1, "q1_size": 1, "q2_size": 2}])
        result = self.cli(results=self.results, occupancy=wrong_header)
        self.assert_diagnostic(result, wrong_header.name, "q1_size")

        write_csv(wrong_header, OCCUPANCY_HEADER, [{"t_ms": 0, "q1_size": 0, "q2_size": 1}])
        result = self.cli(results=self.results, occupancy=wrong_header)
        self.assert_diagnostic(result, wrong_header.name, "samples")

        missing_occupancy = self.results / "missing" / "occ_q16_w4_b65536_t1.csv"
        result = self.cli(results=self.results, occupancy=missing_occupancy)
        self.assert_diagnostic(result, "occ_q16_w4_b65536_t1.csv")

    def test_preexisting_output_and_argparse_status(self):
        out = self.root / "already-there"
        out.mkdir()
        result = self.cli(results=self.results, occupancy=self.occupancy, out=out)
        self.assert_diagnostic(result, str(out), "exists")
        result = self.cli()
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)

    def test_in_process_write_and_rename_failures_clean_only_stage(self):
        import plot as plot_module
        args_root = self.root / "inprocess"
        results, occupancy = make_result_fixture(args_root)
        output_parent = self.root / "external-parent"
        output_parent.mkdir()
        output = output_parent / "graphs"
        sentinel = output_parent / "keep.txt"
        sentinel.write_text("preserve", encoding="utf-8")
        stderr = io.StringIO()
        original_write = plot_module._write_svg
        calls = []

        def fail_after_one_svg(path, root):
            calls.append(path)
            if len(calls) == 2:
                raise OSError("write failed")
            original_write(path, root)

        with mock.patch.object(plot_module, "_write_svg", side_effect=fail_after_one_svg):
            with contextlib.redirect_stderr(stderr):
                status = plot_module.main(["--results", str(results), "--occupancy", str(occupancy),
                                           "--out", str(output)])
        self.assertEqual(status, 1)
        self.assertTrue(stderr.getvalue().startswith("plot: "))
        self.assertEqual(len(stderr.getvalue().splitlines()), 1)
        self.assertFalse(output.exists())
        self.assertEqual(list(output_parent.glob(".graphs.stage-*")), [])
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "preserve")

        stderr = io.StringIO()
        with mock.patch.object(plot_module.os, "rename", side_effect=OSError("rename failed")):
            with contextlib.redirect_stderr(stderr):
                status = plot_module.main(["--results", str(results), "--occupancy", str(occupancy),
                                           "--out", str(output)])
        self.assertEqual(status, 1)
        self.assertTrue(stderr.getvalue().startswith("plot: "))
        self.assertFalse(output.exists())
        self.assertEqual(list(output_parent.glob(".graphs.stage-*")), [])
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "preserve")


if __name__ == "__main__":
    unittest.main()
