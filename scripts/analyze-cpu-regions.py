"""Rank bounded CPU study addresses from one xperf ETL text export.

This is sampling-based triage, not an instruction trace or admission proof.
Input: manifest.json, trace-stats.txt, events.txt produced from the SAME ETL.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import datetime as dt
import json
import math
import re
from collections import Counter, defaultdict
from pathlib import Path


PID_RE = re.compile(r"\((\d+)\)\s*$")
HEX_RE = re.compile(r"0x[0-9a-fA-F]+")
START_RE = re.compile(r"Start time \(UTC\)\s*:\s*(\d{4}/\d\d/\d\d:\d\d:\d\d:\d\d\.\d+)")
LOSS_RE = re.compile(r"Total # Lost (Buffers|Events)\s*:\s*(\d+)")


def pid_of(value: str) -> int | None:
    match = PID_RE.search(value)
    return int(match.group(1)) if match else None


def event_rows(path: Path):
    with path.open(encoding="utf-8-sig", errors="replace", newline="") as stream:
        in_header = False
        for line in stream:
            if line.startswith("BeginHeader"):
                in_header = True
                continue
            if in_header:
                if line.startswith("EndHeader"):
                    in_header = False
                continue
            if not line.lstrip() or "," not in line:
                continue
            row = next(csv.reader([line], skipinitialspace=True), [])
            if len(row) < 3:
                continue
            try:
                timestamp_us = int(row[1].strip())
            except ValueError:
                continue
            yield row[0].strip(), timestamp_us, [cell.strip() for cell in row[2:]]


def trace_metadata(path: Path) -> dict:
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    start = START_RE.search(text)
    losses = {kind.lower(): int(count) for kind, count in LOSS_RE.findall(text)}
    return {
        "trace_start_utc": (
            dt.datetime.strptime(start.group(1).split('.')[0] + '.' + start.group(1).split('.')[1][:6], "%Y/%m/%d:%H:%M:%S.%f")
            .replace(tzinfo=dt.timezone.utc).isoformat()
            if start else None
        ),
        "lost_buffers": losses.get("buffers"),
        "lost_events": losses.get("events"),
        "event_loss_verified": set(losses) == {"buffers", "events"},
    }


def analyze(capture: Path, bucket_size: int = 4096, limit: int = 25) -> dict:
    manifest = json.loads((capture / "manifest.json").read_text(encoding="utf-8-sig"))
    meta = trace_metadata(capture / "trace-stats.txt")
    pid = int(manifest["target_pid"])
    trace_start = dt.datetime.fromisoformat(meta["trace_start_utc"]) if meta["trace_start_utc"] else None
    created = dt.datetime.fromisoformat(manifest["target_creation_utc"].replace("Z", "+00:00"))
    creation_us = max(0, int((created - trace_start).total_seconds() * 1e6)) if trace_start else None
    lifetime_start = creation_us if creation_us is not None else 0
    modules: list[dict] = []
    samples: list[tuple[int, int, int]] = []
    presents: list[int] = []
    gpu_events = 0
    ready: dict[int, int] = {}
    waiting: dict[int, int] = {}
    running: dict[int, int] = {}
    runnable_us = Counter()
    running_us = Counter()
    waiting_us = Counter()
    wait_transitions = Counter()
    process_end_us = None
    pid_reused = False
    sample_overflow = 0
    metadata_overflow = 0
    process_rundown_seen = False
    max_us = 0
    for kind, timestamp, fields in event_rows(capture / "events.txt"):
        max_us = max(max_us, timestamp)
        event_pid = pid_of(fields[0]) if fields else None
        if kind in ("P-DCStart", "P-DCEnd", "P-Start") and event_pid == pid:
            process_rundown_seen = True
            if kind == "P-Start" and creation_us == 0 and timestamp > 0:
                pid_reused = True
                process_end_us = min(process_end_us or timestamp, timestamp)
        elif kind == "P-End" and event_pid == pid:
            process_end_us = timestamp
        elif kind in ("I-Start", "I-DCStart", "I-DCEnd") and event_pid == pid and len(fields) >= 6:
            try:
                base, end = int(fields[1], 16), int(fields[2], 16)
            except ValueError:
                continue
            if base < end:
                if kind == "I-DCEnd" and any(module["base"] == base and module["end_us"] is None for module in modules):
                    continue
                if len(modules) >= 8192:
                    metadata_overflow += 1
                    continue
                modules.append({"base": base, "end": end,
                                "start_us": 0 if kind == "I-DCEnd" else timestamp,
                                "end_us": None, "checksum": fields[3],
                                "image_timestamp": fields[4], "path": fields[6] if len(fields) > 6 else fields[5]})
        elif kind == "I-End" and event_pid == pid and len(fields) >= 3:
            try:
                base = int(fields[1], 16)
            except ValueError:
                continue
            for module in reversed(modules):
                if module["base"] == base and module["end_us"] is None:
                    module["end_us"] = timestamp
                    break
        elif kind == "SampledProfile" and event_pid == pid and len(fields) >= 3:
            try:
                if len(samples) < 2_000_000:
                    samples.append((timestamp, int(fields[1]), int(fields[2], 16)))
                else:
                    sample_overflow += 1
            except ValueError:
                pass
        elif (kind == "ReadyThread" and len(fields) >= 4 and pid_of(fields[2]) == pid
              and timestamp >= lifetime_start and (process_end_us is None or timestamp <= process_end_us)):
            try:
                thread_id = int(fields[3])
                if thread_id in waiting:
                    waiting_us[thread_id] += max(0, timestamp - waiting.pop(thread_id))
                if len(ready) < 8192:
                    ready.setdefault(thread_id, timestamp)
                else:
                    metadata_overflow += 1
            except ValueError:
                pass
        elif (kind == "CSwitch" and len(fields) >= 12
              and timestamp >= lifetime_start and (process_end_us is None or timestamp <= process_end_us)):
            try:
                new_tid, old_tid = int(fields[1]), int(fields[7])
            except ValueError:
                continue
            if pid_of(fields[0]) == pid:
                if new_tid in ready:
                    runnable_us[new_tid] += max(0, timestamp - ready.pop(new_tid))
                if len(running) < 8192 or new_tid in running:
                    running[new_tid] = timestamp
                else:
                    metadata_overflow += 1
            if pid_of(fields[6]) == pid:
                if old_tid in running:
                    running_us[old_tid] += max(0, timestamp - running.pop(old_tid))
                wait_transitions[fields[11]] += 1
                if fields[10] == "Waiting":
                    if len(waiting) < 8192:
                        waiting[old_tid] = timestamp
                    else:
                        metadata_overflow += 1
        elif "Present" in kind and event_pid == pid:
            # Only timestamped events from this ETL, never imported PresentMon rows.
            if "Start" in kind or "Present/" in kind:
                if len(presents) < 2_000_000:
                    presents.append(timestamp)
                else:
                    metadata_overflow += 1
        if ("DmaPacket" in kind or "GpuWork" in kind) and event_pid == pid:
            gpu_events += 1

    lifetime_end = process_end_us if process_end_us is not None else max_us
    modules.sort(key=lambda module: (module["base"], module["start_us"]))
    starts = [module["base"] for module in modules]
    region_hits: dict[tuple, list[int]] = defaultdict(list)
    unattributed = 0
    valid_samples = 0
    for timestamp, thread_id, pc in samples:
        if not lifetime_start <= timestamp <= lifetime_end:
            continue
        valid_samples += 1
        # Loaded images are time scoped. ASLR bases never become identities.
        module = None
        for index in range(bisect.bisect_right(starts, pc) - 1, -1, -1):
            candidate = modules[index]
            if (pc < candidate["end"] and candidate["start_us"] <= timestamp
                    and (candidate["end_us"] is None or timestamp < candidate["end_us"])):
                if module is None or candidate["start_us"] > module["start_us"]:
                    module = candidate
        if module is None:
            unattributed += 1
            continue
        offset = pc - module["base"]
        key = (module["path"], module["checksum"], module["image_timestamp"],
               (offset // bucket_size) * bucket_size)
        if key not in region_hits and len(region_hits) >= 8192:
            metadata_overflow += 1
            continue
        region_hits[key].append(timestamp)

    presents = sorted(set(t for t in presents if lifetime_start <= t <= lifetime_end))
    regions = []
    for (path, checksum, image_timestamp, offset), times in region_hits.items():
        bins = {time // 100_000 for time in times}
        if len(times) < 3 or len(bins) < 3:
            continue
        observed_bins = max(1, math.ceil(max(1, lifetime_end - lifetime_start) / 100_000))
        recurrence = len(bins) / observed_bins
        frame_hits = len({bisect.bisect_right(presents, time) for time in times}) if len(presents) >= 2 else None
        # Ranking only. The execution count, region boundaries and semantics remain unknown.
        score = len(times) * math.sqrt(len(bins)) * (0.5 + recurrence)
        regions.append({
            "module_path": path, "module_checksum": checksum,
            "module_image_timestamp": image_timestamp,
            "offset_begin": f"0x{offset:x}", "offset_end_exclusive": f"0x{offset + bucket_size:x}",
            "sample_hits": len(times), "sampling_share": round(len(times) / max(1, valid_samples), 6),
            "active_100ms_bins": len(bins), "recurrence_fraction": round(recurrence, 6),
            "present_intervals_with_samples": frame_hits, "ranking_score": round(score, 3),
            "criticality": "sampling_estimate_only" if frame_hits is not None else "unavailable",
            "analyzability": "unknown_pending_bounded_instruction_trace",
            "admission": "declined_no_region_semantics_or_dependency_proof",
        })
    regions.sort(key=lambda row: (-row["ranking_score"], row["module_path"], row["offset_begin"]))
    missing = []
    if not meta["event_loss_verified"] or meta["lost_events"] or meta["lost_buffers"]:
        missing.append("zero_event_and_buffer_loss_not_verified")
    if not process_rundown_seen:
        missing.append("target_process_lifetime_not_seen_in_etw")
    if not modules:
        missing.append("target_module_map_missing")
    if len(presents) < 2:
        missing.append("same_etl_frame_boundaries_missing")
    if not gpu_events:
        missing.append("same_etl_gpu_work_events_missing")
    if not samples:
        missing.append("target_cpu_samples_missing")
    if sample_overflow:
        missing.append("sample_budget_exceeded")
    if metadata_overflow:
        missing.append("metadata_budget_exceeded")
    if pid_reused:
        missing.append("target_pid_reused_during_trace")
    if trace_start is None:
        missing.append("trace_clock_origin_missing")
    return {
        "schema": 1, "source": "one_xperf_etl_dump", "target_pid": pid,
        "target_creation_utc": manifest["target_creation_utc"], "clock": {
            "origin_utc": meta["trace_start_utc"], "event_unit": "microseconds_since_trace_start",
            "target_creation_offset_us": creation_us,
        },
        "event_loss": {k: meta[k] for k in ("lost_events", "lost_buffers", "event_loss_verified")},
        "module_identity_strength": "weak_path_checksum_pe_timestamp; no content hash",
        "frame_event_parser_status": "event-name heuristic; provider payload layout needs native validation",
        "process_lifetime": {"rundown_seen": process_rundown_seen,
                             "end_event_offset_us": process_end_us, "pid_reused": pid_reused},
        "counts": {"samples": valid_samples, "unattributed_samples": unattributed,
                   "samples_dropped_by_budget": sample_overflow,
                   "metadata_dropped_by_budget": metadata_overflow,
                   "module_loads": len(modules), "present_events": len(presents),
                   "target_gpu_event_rows": gpu_events},
        "scheduling": {"target_running_us_by_thread": dict(running_us),
                       "target_runnable_us_by_thread": dict(runnable_us),
                       "target_waiting_us_by_thread": dict(waiting_us),
                       "target_switch_out_reasons": dict(wait_transitions),
                       "scope": "observed_intervals_only; boundary intervals omitted"},
        "missing_evidence": missing, "ranking_basis": "at least three hits across three 100ms bins; sample hits and recurrence, no thread names or engine labels",
        "region_bucket_bytes": bucket_size, "regions": regions[:limit],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument("--bucket-bytes", type=int, default=4096)
    parser.add_argument("--limit", type=int, default=25)
    args = parser.parse_args()
    if args.bucket_bytes < 64 or args.bucket_bytes & (args.bucket_bytes - 1):
        parser.error("--bucket-bytes must be a power of two >= 64")
    result = analyze(args.capture, args.bucket_bytes, args.limit)
    output = args.capture / "analysis.json"
    output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"{len(result['regions'])} bounded regions ranked; {len(result['missing_evidence'])} evidence gaps. {output}")


if __name__ == "__main__":
    main()
