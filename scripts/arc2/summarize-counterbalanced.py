"""Summarize successful Present-return cadence inside recorded QPC windows.

These are application Present-return intervals, never display FPS. Missing CPU
submission and GPU spans remain null until measured by independent counters.
"""
from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path, PureWindowsPath

import numpy as np


def rows(path: Path, launch: int, begin: int, end: int,
         end_name: str, freq: int) -> tuple[list[dict[str, int]], int, int]:
    if not path.exists():
        return [], 0, 0
    with path.open(newline="", encoding="utf-8-sig") as file:
        items = [{key: int(value) for key, value in row.items()}
                 for row in csv.DictReader(file)]
    if any(row.get("qpc_frequency") != freq for row in items):
        raise ValueError(f"QPC frequency differs from parent clock: {path}")
    def successful(row: dict[str, int]) -> bool:
        return (row.get("successful_present") == 1 and row.get("hresult") == 0
                and (row.get("flags", 0) & 0x1) == 0)  # DXGI_PRESENT_TEST
    warmup = sum(successful(row) and
                 launch <= row[end_name] < begin for row in items)
    window = [row for row in items if successful(row)
              and begin <= row["begin_qpc"] <= row[end_name] <= end]
    return window, warmup, len(items)


def percentile(values: list[float], p: float) -> float | None:
    return float(np.percentile(values, p)) if values else None


def monotonic(events: list[dict[str, int]], end_name: str) -> bool:
    timestamps = [row[end_name] for row in events]
    return all(right > left for left, right in zip(timestamps, timestamps[1:]))


def summarize_present(events: list[dict[str, int]], end_name: str, freq: int) -> dict:
    ends = [row[end_name] for row in events]
    intervals = [(b - a) * 1000 / freq for a, b in zip(ends, ends[1:]) if b > a]
    duration = [(row[end_name] - row["begin_qpc"]) * 1000 / freq for row in events]
    span = (ends[-1] - ends[0]) / freq if len(ends) > 1 else 0
    return {
        "successful_presents": len(ends),
        "present_return_cadence_hz": (len(ends) - 1) / span if span > 0 else None,
        "present_interval_ms_p50": percentile(intervals, 50),
        "present_interval_ms_p95": percentile(intervals, 95),
        "present_interval_ms_p99": percentile(intervals, 99),
        "present_call_ms_p50": percentile(duration, 50),
        "present_call_ms_p95": percentile(duration, 95),
        "present_call_ms_p99": percentile(duration, 99),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("matrix", type=Path)
    parser.add_argument("--root", type=Path,
                        help="rebase run paths to an extracted matrix directory")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    matrix = json.loads(args.matrix.read_text(encoding="utf-8-sig"))
    order = matrix.get("order", [])
    allowed = (["native", "passthrough", "observe", "optimize",
                "optimize", "observe", "passthrough", "native"],
               ["native", "passthrough", "passthrough", "native"])
    if order not in allowed or len(matrix["runs"]) != matrix["rounds"] * len(order):
        raise ValueError("Matrix must contain complete ABBA or A-B-C-D-D-C-B-A rounds")
    if matrix["warmup_seconds"] < 5 or matrix["seconds"] - matrix["warmup_seconds"] < 10:
        raise ValueError("Matrix needs at least 5 s warmup and 10 s measurement")
    results = []
    by_mode = defaultdict(list)
    identity = None
    frontend_sha = None
    for position, item in enumerate(matrix["runs"]):
        round_expected = position // len(order) + 1
        mode_expected = order[position % len(order)]
        if (item["round"] != round_expected or item["order_index"] != position + 1
                or item["mode"] != mode_expected):
            raise ValueError(f"ABBA order/index mismatch at run {position + 1}")
        original_path = PureWindowsPath(item["run_json"])
        run_path = Path(item["run_json"])
        if args.root:
            run_path = args.root / original_path.parent.name / original_path.name
        elif not run_path.is_absolute():
            if "\\" in item["run_json"] or original_path.drive:
                run_path = args.matrix.parent / original_path.parent.name / original_path.name
            else:
                run_path = args.matrix.parent / run_path
        elif not run_path.exists():
            run_path = args.matrix.parent / original_path.parent.name / original_path.name
        run = json.loads(run_path.read_text(encoding="utf-8-sig"))
        root = run_path.parent
        if run["mode"] != item["mode"] or run["workload"] != matrix["workload"]:
            raise ValueError(f"Run mode/workload differs from matrix: {run_path}")
        if (run.get("binary_hash_stable") is False or run.get("repo_head_stable") is False or
                (run.get("executable_sha256_after") and
                 run["executable_sha256_after"] != run["executable_sha256"]) or
                (run.get("frontend_sha256_after") and
                 run["frontend_sha256_after"] != run.get("frontend_sha256")) or
                (run.get("arc_commit_after") and run["arc_commit_after"] != run["arc_commit"])):
            raise ValueError(f"Binary or repository changed during run: {run_path}")
        normalized_args = []
        for value in run["arguments"]:
            arg = str(value)
            if run["testbed"] == "cauldron" and arg.startswith("path="):
                if PureWindowsPath(arg[5:]).name != run_path.parent.name:
                    raise ValueError(f"Cauldron output path differs from run dir: {run_path}")
                arg = "path={OUT}"
            normalized_args.append(arg)
        normalized_args = tuple(normalized_args)
        current_identity = (run["testbed"], run["workload"], run["executable_sha256"],
                            run["working_directory"], normalized_args,
                            json.dumps(run.get("environment", {}), sort_keys=True),
                            run["arc_commit"])
        if identity is None:
            identity = current_identity
        elif current_identity != identity:
            raise ValueError(f"Cross-arm executable/workload/settings/commit mismatch: {run_path}")
        if run["mode"] != "native":
            if not run.get("frontend_sha256"):
                raise ValueError(f"Missing frontend DLL hash: {run_path}")
            if frontend_sha is None:
                frontend_sha = run["frontend_sha256"]
            elif run["frontend_sha256"] != frontend_sha:
                raise ValueError(f"Frontend DLL changed within matrix: {run_path}")
        begin = run["measurement_start_qpc"]
        end = run["measurement_end_qpc"]
        freq = run["qpc_frequency"]
        launch = run["process_start_qpc"]
        native, native_warmup, native_total = rows(root / "native.frames.csv", launch, begin,
                                     end, "end_qpc", freq)
        frontend, frontend_warmup, frontend_total = rows(root / "arc2.json.frames.csv", launch,
                                         begin, end, "wrapper_end_qpc", freq)
        native_not_capped = native_total < 131072  # native-present-timer.hpp capacity
        row_counts_match = native_total == run.get("native_present_rows")
        frontend_lossless = run["mode"] == "native"
        if run["mode"] != "native":
            meta_path = root / "arc2.json.frames.meta.json"
            if meta_path.exists():
                meta = json.loads(meta_path.read_text(encoding="utf-8-sig"))
                frontend_lossless = (meta.get("dropped") == 0 and
                                     meta.get("rows") == frontend_total and
                                     frontend_total == run.get("present_rows"))
        native_summary = summarize_present(native, "end_qpc", freq)
        frontend_summary = summarize_present(frontend, "wrapper_end_qpc", freq)
        native_cadence = native_summary["present_return_cadence_hz"]
        frontend_cadence = frontend_summary["present_return_cadence_hz"]
        wrapper_us = [(row["wrapper_end_qpc"] - row["native_end_qpc"]) * 1e6 / freq
                      for row in frontend if row["wrapper_end_qpc"] >= row["native_end_qpc"]]
        result = {
            "round": item["round"], "order_index": item["order_index"],
            "mode": item["mode"], "run_json": str(run_path),
            "status": "measured" if (run["eligible_for_timing_analysis"] and
                native_not_capped and row_counts_match and frontend_lossless and
                len(native) > 1 and native_warmup >= 2 and
                monotonic(native, "end_qpc") and native_cadence is not None and native_cadence > 0 and
                (run["mode"] == "native" or (len(frontend) > 1 and frontend_warmup >= 2 and
                 monotonic(frontend, "wrapper_end_qpc") and frontend_cadence is not None and frontend_cadence > 0)) and
                (end - begin) / freq >= 10) else "invalid",
            "window_seconds": (end - begin) / freq,
            "warmup_successful_native_presents": native_warmup,
            "warmup_successful_frontend_presents": frontend_warmup if run["mode"] != "native" else None,
            "native_recorder_cap_reached": not native_not_capped,
            "frontend_present_recording_lossless": frontend_lossless if run["mode"] != "native" else None,
            "native_app_present": native_summary,
            "frontend_present": frontend_summary if run["mode"] != "native" else None,
            "frontend_present_wrapper_us_p50": percentile(wrapper_us, 50),
            "cpu_processing_to_submission_ms": None,
            "gpu_frame_span_ms": None,
            "display_fps": None,
        }
        results.append(result)
        if result["status"] == "measured":
            by_mode[result["mode"]].append(native_summary["present_return_cadence_hz"])
    brackets = {}
    adjusted = defaultdict(list)
    for round_no in sorted({result["round"] for result in results}):
        series = sorted((r for r in results if r["round"] == round_no),
                        key=lambda r: r["order_index"])
        if len(series) != len(order) or series[0]["mode"] != "native" or series[-1]["mode"] != "native":
            continue
        if series[0]["status"] != "measured" or series[-1]["status"] != "measured":
            continue
        first = series[0]["native_app_present"]["present_return_cadence_hz"]
        last = series[-1]["native_app_present"]["present_return_cadence_hz"]
        if first is None or last is None or first <= 0:
            continue
        brackets[str(round_no)] = 100 * (last / first - 1)
        for index, run in enumerate(series[1:-1], start=1):
            observed = run["native_app_present"]["present_return_cadence_hz"]
            expected = first + (last - first) * index / (len(series) - 1)
            if run["status"] == "measured" and observed is not None and expected > 0:
                change = 100 * (observed / expected - 1)
                run["native_bracket_interpolated_cadence_hz"] = expected
                run["native_bracket_adjusted_change_percent"] = change
                adjusted[run["mode"]].append(change)
    raw_medians = {
        mode: percentile([x for x in values if x is not None], 50)
        for mode, values in by_mode.items()
    }
    complete = len(results) == matrix["rounds"] * len(order) and all(
        run["status"] == "measured" for run in results
    )
    summary = {
        "schema": "arc2-counterbalanced-summary-v1",
        "workload": matrix["workload"],
        "measurement": "successful application Present-return cadence; not display FPS",
        "rounds": matrix["rounds"],
        "executable_sha256": identity[2] if identity else None,
        "frontend_sha256": frontend_sha,
        "arc_commit": identity[-1] if identity else None,
        "warmup_anchor": "parent QPC before process creation; measured rows require at least two successful warmup Presents",
        "mode_cadence_median_hz": raw_medians if complete else {},
        "raw_unpaired_mode_cadence_median_hz": raw_medians,
        "complete_measured_brackets": complete,
        "native_bracket_drift_percent_by_round": brackets,
        "native_bracket_adjusted_median_percent": {
            mode: percentile(values, 50) for mode, values in adjusted.items()
        },
        "runs": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary["mode_cadence_median_hz"]))


if __name__ == "__main__":
    main()
