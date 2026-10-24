#!/usr/bin/env python3
"""Render the four W3-02 benchmark graphs using only Python's standard library."""

import argparse
import csv
import math
import os
import re
import shutil
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path


SVG_NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG_NS)
TAG = lambda name: f"{{{SVG_NS}}}{name}"
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
NUMERIC_FIELDS = {
    "queue_cap", "workers", "block_bytes", "trial", "input_bytes", "out_rate_mb_s",
    "bytes", "blocks", "wall_s", "throughput_mb_s", "lat_p50_us", "lat_p99_us",
    "cpu_user_s", "cpu_sys_s", "peak_rss_kb", "io_call_s",
}
INTEGER_FIELDS = {"queue_cap", "workers", "block_bytes", "trial", "input_bytes", "bytes", "blocks"}
MEDIAN_KEY = (
    "run_group", "mode", "queue_cap", "workers", "block_bytes", "input_bytes",
    "cache_method", "out_rate_mb_s",
)
OCCUPANCY_RE = re.compile(r"^occ_q([0-9]+)_w([0-9]+)_b([0-9]+)_t1\.csv$")


class InputError(Exception):
    """An input/validation error suitable for a one-line CLI diagnostic."""


def _finite_number(filename, column, value, *, allow_empty=False):
    if value is None or value == "":
        if allow_empty:
            return None
        raise InputError(f"{filename}: missing value for column {column}")
    try:
        number = float(value)
    except (TypeError, ValueError):
        raise InputError(f"{filename}: malformed {column} value {value!r}") from None
    if not math.isfinite(number):
        raise InputError(f"{filename}: malformed {column} value {value!r}")
    return number


def _integer(filename, column, value, *, allow_empty=False, positive=False, nonnegative=False):
    number = _finite_number(filename, column, value, allow_empty=allow_empty)
    if number is None:
        return None
    if not number.is_integer() or (positive and number <= 0) or (nonnegative and number < 0):
        raise InputError(f"{filename}: malformed {column} value {value!r}")
    return int(number)


def _canonical_number(filename, column, value, *, allow_empty=False, integer=False,
                      positive=True, nonnegative=False):
    if value is None or value == "":
        if allow_empty:
            return ""
        raise InputError(f"{filename}: missing value for column {column}")
    if integer:
        return str(_integer(filename, column, value, positive=positive, nonnegative=nonnegative))
    number = _finite_number(filename, column, value)
    return format(number, ".15g")


def _read_csv(path, required_header):
    filename = path.name
    if not path.is_file():
        raise InputError(f"{filename}: missing file {path}")
    try:
        with path.open("r", encoding="utf-8", newline="") as stream:
            reader = csv.DictReader(stream)
            header = reader.fieldnames
            if header is None:
                raise InputError(f"{filename}: missing header")
            missing = [field for field in required_header if field not in header]
            if missing:
                raise InputError(f"{filename}: missing column {missing[0]}")
            if header != required_header:
                extra = next((field for field in header if field not in required_header), None)
                if extra:
                    raise InputError(f"{filename}: unexpected column {extra}")
                raise InputError(f"{filename}: unexpected column order near {header[0]!r}")
            rows = []
            for row_number, row in enumerate(reader, 2):
                if None in row:
                    raise InputError(f"{filename}: malformed row {row_number} (extra fields)")
                missing_values = [field for field in required_header if row.get(field) is None]
                if missing_values:
                    raise InputError(
                        f"{filename}: malformed row {row_number}, missing value for column "
                        f"{missing_values[0]}"
                    )
                rows.append(row)
            return rows
    except UnicodeError as error:
        raise InputError(f"{filename}: cannot decode CSV ({error})") from None
    except csv.Error as error:
        raise InputError(f"{filename}: malformed CSV ({error})") from None
    except OSError as error:
        raise InputError(f"{filename}: cannot read {path}: {error.strerror or error}") from None


def _validate_group(row, filename):
    group = row["run_group"]
    mode = row["mode"]
    if group == "core" and mode == "pipeline":
        cap = _integer(filename, "queue_cap", row["queue_cap"], positive=True)
        if cap not in (1, 4, 16, 64):
            raise InputError(f"{filename}: malformed queue_cap value {row['queue_cap']!r}")
    elif group == "baseline" and mode == "blocking":
        if row["queue_cap"] != "":
            raise InputError(f"{filename}: malformed queue_cap value {row['queue_cap']!r}")
    elif group == "memory" and mode in ("bounded", "unbounded"):
        _integer(filename, "queue_cap", row["queue_cap"], positive=True)
        _finite_number(filename, "out_rate_mb_s", row["out_rate_mb_s"])
        if float(row["out_rate_mb_s"]) <= 0:
            raise InputError(f"{filename}: malformed out_rate_mb_s value {row['out_rate_mb_s']!r}")
    else:
        raise InputError(f"{filename}: malformed run_group/mode value {group!r}/{mode!r}")
    _integer(filename, "workers", row["workers"], positive=True)
    _integer(filename, "block_bytes", row["block_bytes"], positive=True)
    _integer(filename, "input_bytes", row["input_bytes"], nonnegative=True)
    if row["cache_method"] not in ("warm", "larger", "drop"):
        raise InputError(f"{filename}: malformed cache_method value {row['cache_method']!r}")
    if group == "baseline":
        if _integer(filename, "workers", row["workers"], positive=True) != 1:
            raise InputError(f"{filename}: malformed workers value {row['workers']!r}")
        if row["out_rate_mb_s"] != "":
            raise InputError(f"{filename}: malformed out_rate_mb_s value {row['out_rate_mb_s']!r}")
    if group == "core" and row["out_rate_mb_s"] != "":
        raise InputError(f"{filename}: malformed out_rate_mb_s value {row['out_rate_mb_s']!r}")


def _validate_rows(rows, filename, *, is_runs):
    for row_number, row in enumerate(rows, 2):
        _validate_group(row, filename)
        if is_runs:
            if row["status"] not in ("success", "failed"):
                raise InputError(f"{filename}: malformed status value {row['status']!r}")
            _integer(filename, "trial", row["trial"], positive=True)
        for field in NUMERIC_FIELDS:
            if field in row and field not in ("queue_cap", "workers", "block_bytes", "input_bytes", "trial"):
                may_be_empty = (is_runs or field not in MEDIAN_KEY or
                                (field == "out_rate_mb_s" and row["run_group"] != "memory"))
                _finite_number(filename, field, row[field], allow_empty=may_be_empty)
        if not is_runs:
            for field in ("bytes", "blocks", "wall_s", "throughput_mb_s", "lat_p50_us",
                          "lat_p99_us", "cpu_user_s", "cpu_sys_s", "peak_rss_kb", "io_call_s"):
                _finite_number(filename, field, row[field], allow_empty=True)


def _normal_key(row, filename):
    key = []
    for field in MEDIAN_KEY:
        value = row[field]
        if field in ("queue_cap", "workers", "block_bytes", "input_bytes"):
            key.append(_canonical_number(filename, field, value, allow_empty=field == "queue_cap",
                                         integer=True, positive=field != "input_bytes",
                                         nonnegative=field == "input_bytes"))
        elif field == "out_rate_mb_s":
            key.append(_canonical_number(filename, field, value, allow_empty=True))
        else:
            key.append(value)
    return tuple(key)


def _read_occupancy(path):
    match = OCCUPANCY_RE.fullmatch(path.name)
    if not match:
        raise InputError(f"{path.name}: malformed occupancy filename; expected occ_q<Q>_w<W>_b<B>_t1.csv")
    config = tuple(int(value) for value in match.groups())
    if any(value <= 0 for value in config):
        raise InputError(f"{path.name}: malformed occupancy filename values")
    rows = _read_csv(path, OCCUPANCY_HEADER)
    if len(rows) < 2:
        raise InputError(f"{path.name}: requires at least two samples")
    samples = []
    prior_t = None
    for row_number, row in enumerate(rows, 2):
        sample = {}
        for field in OCCUPANCY_HEADER:
            sample[field] = _finite_number(path.name, field, row[field])
        if sample["t_ms"] < 0:
            raise InputError(f"{path.name}: malformed t_ms value {row['t_ms']!r}")
        if sample["q1_size"] < 0 or not sample["q1_size"].is_integer():
            raise InputError(f"{path.name}: malformed q1_size value {row['q1_size']!r}")
        if sample["q2_size"] < 0 or not sample["q2_size"].is_integer():
            raise InputError(f"{path.name}: malformed q2_size value {row['q2_size']!r}")
        if prior_t is not None and sample["t_ms"] < prior_t:
            raise InputError(f"{path.name}: malformed t_ms value {row['t_ms']!r} (decreasing timestamp)")
        prior_t = sample["t_ms"]
        samples.append(sample)
    return {"config": config, "samples": samples, "path": path}


def _load_inputs(results_path, occupancy_path):
    if not results_path.is_dir():
        raise InputError(f"runs.csv: missing result directory {results_path}")
    runs_path = results_path / "runs.csv"
    medians_path = results_path / "medians.csv"
    runs = _read_csv(runs_path, RUNS_HEADER)
    medians = _read_csv(medians_path, MEDIANS_HEADER)
    _validate_rows(runs, "runs.csv", is_runs=True)
    _validate_rows(medians, "medians.csv", is_runs=False)
    occupancy = _read_occupancy(occupancy_path)
    keyed = {}
    for row in medians:
        key = _normal_key(row, "medians.csv")
        if key in keyed:
            raise InputError(f"medians.csv: duplicate group key {_format_key(key)}")
        keyed[key] = row
    return {"runs": runs, "medians": medians, "keyed": keyed, "occupancy": occupancy,
            "results_path": results_path}


def _format_key(key):
    return " ".join(f"{field}={value}" for field, value in zip(MEDIAN_KEY, key))


WIDTH = 900
HEIGHT = 680
PLOT_LEFT = 95
PLOT_RIGHT = 850
PLOT_TOP = 105
PLOT_BOTTOM = 470
LINE_STYLES = (
    ("#1f77b4", "circle", "solid"),
    ("#ff7f0e", "square", "solid"),
    ("#2ca02c", "diamond", "solid"),
    ("#d62728", "triangle", "solid"),
    ("#9467bd", "circle", "dashed"),
    ("#8c564b", "square", "dashed"),
    ("#e377c2", "diamond", "dashed"),
    ("#17becf", "triangle", "dashed"),
)


def _svg_element(parent, name, **attrs):
    converted = {}
    for key, value in attrs.items():
        attr = "class" if key == "class_" else key.replace("_", "-")
        converted[attr] = str(value)
    return ET.SubElement(parent, TAG(name), converted)


def _add_text(parent, x, y, content, role, **attrs):
    text_attrs = {"x": x, "y": y, "data-role": role}
    text_attrs.update(attrs)
    node = _svg_element(parent, "text", **text_attrs)
    node.text = str(content)
    return node


def _new_svg(title, x_label, y_label):
    root = ET.Element(TAG("svg"), {
        "version": "1.1", "width": str(WIDTH), "height": str(HEIGHT),
        "viewBox": f"0 0 {WIDTH} {HEIGHT}",
    })
    _add_text(root, WIDTH / 2, 38, title, "title", **{"text-anchor": "middle", "font-size": 22})
    _add_text(root, (PLOT_LEFT + PLOT_RIGHT) / 2, 645, x_label, "x-label",
              **{"text-anchor": "middle", "font-size": 15})
    _add_text(root, 24, (PLOT_TOP + PLOT_BOTTOM) / 2, y_label, "y-label",
              transform=f"rotate(-90 24 {(PLOT_TOP + PLOT_BOTTOM) / 2})",
              **{"text-anchor": "middle", "font-size": 15})
    return root


def _numeric_range(values, *, include_zero=False, nonnegative=False):
    low = min(values)
    high = max(values)
    if include_zero:
        low = min(0.0, low)
        high = max(0.0, high)
    if low == high:
        padding = max(abs(low) * 0.1, 1.0)
        low -= padding
        high += padding
    if nonnegative and low < 0:
        low = 0.0
    if nonnegative and high == 0:
        high = 1.0
    if high <= low:
        high = low + 1.0
    return low, high


def _map_linear(value, low, high, pixel_low, pixel_high):
    if high <= low:
        low, high = _numeric_range([low, high])
    return pixel_low + (value - low) / (high - low) * (pixel_high - pixel_low)


def _format_tick(value):
    if abs(value - round(value)) < 1e-9:
        return str(int(round(value)))
    return f"{value:.3g}"


def _draw_axes(root, x_low, x_high, y_low, y_high, *, x_ticks=None, x_transform=None,
               x_tick_labels=None, y_ticks=5):
    if x_transform is None:
        transform = lambda value: value
        tx_low, tx_high = x_low, x_high
    else:
        transform = x_transform
        tx_low, tx_high = transform(x_low), transform(x_high)
    _svg_element(root, "path", d=f"M {PLOT_LEFT} {PLOT_TOP} V {PLOT_BOTTOM} H {PLOT_RIGHT}",
                 fill="none", stroke="#222", stroke_width="1.5")
    for index in range(y_ticks + 1):
        raw = y_low + (y_high - y_low) * index / y_ticks
        y = _map_linear(raw, y_low, y_high, PLOT_BOTTOM, PLOT_TOP)
        _svg_element(root, "path", d=f"M {PLOT_LEFT} {y:.3f} H {PLOT_RIGHT}",
                     fill="none", stroke="#dddddd", stroke_width="1")
        _add_text(root, PLOT_LEFT - 10, y + 4, _format_tick(raw), "y-label",
                  **{"text-anchor": "end", "font-size": 11})
    ticks = x_ticks if x_ticks is not None else [x_low + (x_high - x_low) * i / 4 for i in range(5)]
    for tick_index, raw in enumerate(ticks):
        if x_tick_labels is not None:
            label = x_tick_labels[tick_index]
        else:
            label = _format_tick(raw)
        mapped = transform(raw)
        x = _map_linear(mapped, tx_low, tx_high, PLOT_LEFT, PLOT_RIGHT)
        _svg_element(root, "path", d=f"M {x:.3f} {PLOT_BOTTOM} V {PLOT_BOTTOM + 5}",
                     fill="none", stroke="#222", stroke_width="1")
        _add_text(root, x, PLOT_BOTTOM + 22, label, "x-label",
                  **{"text-anchor": "middle", "font-size": 11})


def _marker_shape(parent, marker, x, y, color, data_x=None, data_y=None):
    common = {"fill": color, "stroke": color, "stroke_width": 1.2}
    if data_x is not None:
        common["data_x"] = data_x
    if data_y is not None:
        common["data_y"] = data_y
    if marker == "square":
        return _svg_element(parent, "rect", x=x - 4, y=y - 4, width=8, height=8, **common)
    if marker == "diamond":
        return _svg_element(parent, "path", d=f"M {x} {y-5} L {x+5} {y} L {x} {y+5} L {x-5} {y} Z", **common)
    if marker == "triangle":
        return _svg_element(parent, "path", d=f"M {x} {y-5} L {x+5} {y+4} L {x-5} {y+4} Z", **common)
    return _svg_element(parent, "circle", cx=x, cy=y, r=4, **common)


def _line_group(root, series_name, points, style, x_mapper, y_mapper):
    color, marker, dash = style
    group = _svg_element(root, "g", class_="series", data_series=series_name,
                         data_color=color, data_marker=marker, data_dash=dash,
                         stroke=color, fill="none")
    commands = []
    for index, point in enumerate(points):
        px, py = x_mapper(point[0]), y_mapper(point[1])
        commands.append(("M" if index == 0 else "L") + f" {px:.3f} {py:.3f}")
    dash_array = "none" if dash == "solid" else "8,5"
    _svg_element(group, "path", d=" ".join(commands), fill="none", stroke=color,
                 stroke_width="2.5", stroke_dasharray=dash_array,
                 **{"vector-effect": "non-scaling-stroke"})
    for raw_x, raw_y in points:
        _marker_shape(group, marker, x_mapper(raw_x), y_mapper(raw_y), color,
                      data_x=raw_x, data_y=raw_y)
    return group


def _legend(root, entries, *, x=110, y=505, columns=3):
    for index, (label, style) in enumerate(entries):
        color, marker, dash = style
        column = index % columns
        row = index // columns
        x0 = x + column * 250
        y0 = y + row * 23
        group = _svg_element(root, "g", class_="legend-series", data_series=label,
                             data_marker=marker, data_dash=dash, stroke=color, fill="none")
        _svg_element(group, "path", d=f"M {x0} {y0-4} h 28", fill="none", stroke=color,
                     stroke_width="2.5", stroke_dasharray="none" if dash == "solid" else "8,5")
        _marker_shape(group, marker, x0 + 14, y0 - 4, color)
        _add_text(root, x0 + 36, y0, label, "legend", **{"font-size": 12})


def _baseline_group(root, series_name, value, style, x_mapper, y_mapper):
    color, marker, dash = style
    group = _svg_element(root, "g", class_="series", data_series=series_name,
                         data_color=color, data_marker=marker, data_dash=dash,
                         stroke=color, fill="none")
    y = y_mapper(value)
    _svg_element(group, "path", d=f"M {x_mapper(1):.3f} {y:.3f} H {x_mapper(8):.3f}",
                 fill="none", stroke=color, stroke_width="2.5", stroke_dasharray="8,5")
    _marker_shape(group, marker, x_mapper(1), y, color, data_x=1, data_y=value)
    return group


def _assert_unique_line_styles(series):
    combinations = [(item["style"][0], item["style"][1], item["style"][2]) for item in series]
    if len(combinations) != len(set(combinations)):
        raise RuntimeError("duplicate line-series style tuple")


def _required_metric(row, column):
    value = row.get(column, "")
    if value == "":
        raise InputError(f"medians.csv: missing value for column {column} in {_format_key(_normal_key(row, 'medians.csv'))}")
    number = _finite_number("medians.csv", column, value)
    if number < 0:
        raise InputError(f"medians.csv: malformed {column} value {value!r}")
    return number


def _select_unique(data, *, group, mode, cap=None, workers=None, block=None,
                   input_bytes=None, cache_method=None):
    matches = []
    for row in data["medians"]:
        if row["run_group"] != group or row["mode"] != mode:
            continue
        if cap is not None and (row["queue_cap"] or "") != str(cap):
            continue
        if workers is not None and int(row["workers"]) != workers:
            continue
        if block is not None and int(row["block_bytes"]) != block:
            continue
        if input_bytes is not None and int(row["input_bytes"]) != input_bytes:
            continue
        if cache_method is not None and row["cache_method"] != cache_method:
            continue
        matches.append(row)
    if not matches:
        fields = [f"run_group={group}", f"mode={mode}"]
        if cap is not None:
            fields.append(f"queue_cap={cap}")
        if workers is not None:
            fields.append(f"workers={workers}")
        if block is not None:
            fields.append(f"block_bytes={block}")
        if input_bytes is not None:
            fields.append(f"input_bytes={input_bytes}")
        if cache_method is not None:
            fields.append(f"cache_method={cache_method}")
        raise InputError(f"medians.csv: missing required point {' '.join(fields)}")
    if len(matches) != 1:
        key = _format_key(_normal_key(matches[0], "medians.csv"))
        raise InputError(f"medians.csv: duplicate or ambiguous required point {key}")
    return matches[0]


def _series_color_index(block):
    return {4096: 0, 65536: 1, 1048576: 2}[block]


def _render_line_chart(title, x_label, y_label, all_x, series, *, x_ticks=None,
                       x_transform=None, x_tick_labels=None):
    root = _new_svg(title, x_label, y_label)
    x_low, x_high = min(all_x), max(all_x)
    tx_low, tx_high = ((x_transform(x_low), x_transform(x_high)) if x_transform
                       else (x_low, x_high))
    if tx_low == tx_high:
        tx_low, tx_high = _numeric_range([tx_low, tx_high])
    y_values = [point[1] for item in series for point in item["points"]]
    y_values.extend(item["baseline"] for item in series if "baseline" in item)
    y_low, y_high = _numeric_range(y_values, include_zero=True, nonnegative=True)
    _draw_axes(root, x_low, x_high, y_low, y_high, x_ticks=x_ticks,
               x_transform=x_transform, x_tick_labels=x_tick_labels)

    def x_map(value):
        mapped = x_transform(value) if x_transform else value
        return _map_linear(mapped, tx_low, tx_high, PLOT_LEFT, PLOT_RIGHT)

    def y_map(value):
        return _map_linear(value, y_low, y_high, PLOT_BOTTOM, PLOT_TOP)

    _assert_unique_line_styles(series)
    legend_entries = []
    for item in series:
        style = item["style"]
        if "baseline" in item:
            _baseline_group(root, item["label"], item["baseline"], style, x_map, y_map)
        else:
            _line_group(root, item["label"], item["points"], style, x_map, y_map)
        legend_entries.append((item["label"], style))
    _legend(root, legend_entries, x=110, y=505, columns=3)
    return root


def _render_throughput(data):
    core_ref = _select_unique(data, group="core", mode="pipeline", cap=16,
                              workers=1, block=4096)
    reference_input = int(core_ref["input_bytes"])
    reference_cache = core_ref["cache_method"]
    series = []
    for block in (4096, 65536, 1048576):
        style = LINE_STYLES[_series_color_index(block)]
        points = []
        for worker in (1, 2, 4, 8):
            row = _select_unique(data, group="core", mode="pipeline", cap=16,
                                 workers=worker, block=block, input_bytes=reference_input,
                                 cache_method=reference_cache)
            points.append((worker, _required_metric(row, "throughput_mb_s")))
        series.append({"label": f"pipeline, block={block} B", "points": points, "style": style})
    for block in (4096, 65536, 1048576):
        row = _select_unique(data, group="baseline", mode="blocking", cap="",
                             workers=1, block=block, input_bytes=reference_input,
                             cache_method=reference_cache)
        style = (LINE_STYLES[_series_color_index(block)][0],
                 LINE_STYLES[_series_color_index(block)][1], "dashed")
        series.append({"label": f"blocking baseline, block={block} B", "baseline":
                       _required_metric(row, "throughput_mb_s"), "points": [], "style": style})
    return _render_line_chart("Throughput versus worker count", "Workers", "Throughput (MB/s)",
                              [1, 2, 4, 8], series, x_ticks=[1, 2, 4, 8])


def _render_latency(data):
    core_ref = _select_unique(data, group="core", mode="pipeline", cap=16,
                              workers=1, block=65536)
    reference_input = int(core_ref["input_bytes"])
    reference_cache = core_ref["cache_method"]
    series = []
    for worker_index, worker in enumerate((1, 2, 4, 8)):
        for field, percentile in (("lat_p50_us", "p50"), ("lat_p99_us", "p99")):
            base_style = LINE_STYLES[worker_index]
            style = (base_style[0], "circle" if percentile == "p50" else "square",
                     "solid" if percentile == "p50" else "dashed")
            points = []
            for cap in (1, 4, 16, 64):
                row = _select_unique(data, group="core", mode="pipeline", cap=cap,
                                     workers=worker, block=65536,
                                     input_bytes=reference_input, cache_method=reference_cache)
                points.append((cap, _required_metric(row, field)))
            series.append({"label": f"workers={worker} {percentile}",
                           "points": points, "style": style})
    return _render_line_chart("Latency versus queue capacity", "Queue capacity (records)",
                              "Latency (µs)", [1, 4, 16, 64], series,
                              x_ticks=[1, 4, 16, 64], x_transform=math.log10,
                              x_tick_labels=["1", "4", "16", "64"])


def _render_occupancy(data):
    occupancy = data["occupancy"]
    cap, workers, block = occupancy["config"]
    samples = occupancy["samples"]
    times = [sample["t_ms"] for sample in samples]
    series = []
    for index, field in enumerate(("q1_size", "q2_size")):
        name = f"Q{index + 1} occupancy"
        points = [(sample["t_ms"], sample[field]) for sample in samples]
        series.append({"label": name, "points": points, "style": LINE_STYLES[index]})
    title = f"Queue occupancy over time (queue={cap}, workers={workers}, block={block} B)"
    return _render_line_chart(title, "Time (ms)", "Queue size (records)", times, series)


def _memory_pair(data):
    bounded = [row for row in data["medians"]
               if row["run_group"] == "memory" and row["mode"] == "bounded"]
    unbounded = [row for row in data["medians"]
                 if row["run_group"] == "memory" and row["mode"] == "unbounded"]
    if len(bounded) != 1:
        raise InputError("medians.csv: missing or ambiguous required point run_group=memory mode=bounded")
    if len(unbounded) != 1:
        raise InputError("medians.csv: missing or ambiguous required point run_group=memory mode=unbounded")
    left, right = bounded[0], unbounded[0]
    for field in ("input_bytes", "workers", "block_bytes", "cache_method", "out_rate_mb_s", "queue_cap"):
        if _canonical_key_field(left, field) != _canonical_key_field(right, field):
            raise InputError(f"medians.csv: memory pair mismatch on {field}")
    return left, right


def _canonical_key_field(row, field):
    if field in ("input_bytes", "workers", "block_bytes", "queue_cap"):
        return _canonical_number("medians.csv", field, row[field], integer=True,
                                 positive=field != "input_bytes", nonnegative=field == "input_bytes")
    if field == "out_rate_mb_s":
        return _canonical_number("medians.csv", field, row[field])
    return row[field]


def _render_memory(data):
    bounded, unbounded = _memory_pair(data)
    rows = (("bounded", bounded, "#2878b5"), ("unbounded", unbounded, "#e07a25"))
    values = []
    for label, row, fill in rows:
        kb = _required_metric(row, "peak_rss_kb")
        values.append((label, row, fill, kb * 1024.0 / 1_000_000.0))
    low, high = _numeric_range([item[3] for item in values], include_zero=True, nonnegative=True)
    root = _new_svg("Bounded versus unbounded peak memory", "Memory mode", "Peak RSS (decimal MB)")
    _draw_axes(root, -0.5, 1.5, low, high, x_ticks=[0, 1], x_tick_labels=["bounded", "unbounded"])
    centers = (PLOT_LEFT + (PLOT_RIGHT - PLOT_LEFT) / 4,
               PLOT_LEFT + 3 * (PLOT_RIGHT - PLOT_LEFT) / 4)
    width = 150
    styles = []
    for index, (label, _row, fill, value) in enumerate(values):
        marker = "bar"
        dash = "none"
        group = _svg_element(root, "g", class_="series", data_series=label,
                             data_marker=marker, data_dash=dash, stroke="#333333", fill=fill)
        y = _map_linear(value, low, high, PLOT_BOTTOM, PLOT_TOP)
        rect = _svg_element(group, "rect", x=centers[index] - width / 2, y=y,
                            width=width, height=PLOT_BOTTOM - y, fill=fill, stroke="#333333",
                            stroke_width="1.2", data_x=label, data_y=value, data_value=value)
        _add_text(root, centers[index], y - 10, f"{value:.3f} MB", "legend",
                  data_value=f"{value:.12g}", **{"text-anchor": "middle", "font-size": 14})
        _add_text(root, centers[index], PLOT_BOTTOM + 40, label, "legend",
                  data_series=label, **{"text-anchor": "middle", "font-size": 14})
        styles.append((fill, label))
    if len({fill for fill, _ in styles}) != 2 or len({label for _, label in styles}) != 2:
        raise RuntimeError("memory bars need distinct fill and label")
    return root


def _render_all(data):
    return {
        "throughput_vs_workers.svg": _render_throughput(data),
        "latency_vs_queue.svg": _render_latency(data),
        "occupancy_over_time.svg": _render_occupancy(data),
        "memory_bounded_vs_unbounded.svg": _render_memory(data),
    }


def _write_svg(path, root):
    path.write_bytes(ET.tostring(root, encoding="utf-8", xml_declaration=True))


def _render_and_publish(data, output_path):
    figures = _render_all(data)
    parent = output_path.parent
    parent.mkdir(parents=True, exist_ok=True)
    stage = None
    try:
        stage = Path(tempfile.mkdtemp(prefix=f".{output_path.name}.stage-", dir=parent))
        for filename, root in figures.items():
            _write_svg(stage / filename, root)
        if os.path.lexists(output_path):
            raise FileExistsError(f"output path already exists: {output_path}")
        os.rename(stage, output_path)
    except OSError:
        if stage is not None:
            shutil.rmtree(stage, ignore_errors=True)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(prog="plot.py", description="Create W3-02 benchmark plots")
    parser.add_argument("--results", required=True, help="W3-01 result directory")
    parser.add_argument("--occupancy", required=True, help="retained W2-06 occupancy CSV")
    parser.add_argument("--out", help="new output directory (default: DIR/plots)")
    args = parser.parse_args(argv)
    results_path = Path(args.results)
    occupancy_path = Path(args.occupancy)
    output_path = Path(args.out) if args.out is not None else results_path / "plots"
    try:
        if os.path.lexists(output_path):
            raise InputError(f"{output_path}: output path already exists")
        data = _load_inputs(results_path, occupancy_path)
        _render_and_publish(data, output_path)
        print(str(output_path))
        return 0
    except InputError as error:
        print(f"plot: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        detail = error.strerror or str(error)
        print(f"plot: {output_path}: {detail}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
