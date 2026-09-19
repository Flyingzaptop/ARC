"""Full-frame capacity experiment: independently recompute images and GPU ticks."""
import argparse
import json
import math
import statistics as stats
from pathlib import Path
import numpy as np


def image(path):
    raw = np.fromfile(path, dtype=np.uint8)
    if raw.size not in (1920 * 1080 * 3, 3840 * 2160 * 3):
        raise ValueError(f"Incomplete 1080p RGB image: {path}")
    # Match the core's normalized float32 RGB contract, then compute in float64.
    shape = (1080, 1920, 3) if raw.size == 1920 * 1080 * 3 else (2160, 3840, 3)
    return (raw.astype(np.float32) / np.float32(255)).astype(np.float64).reshape(shape)


def compare(a, b, c):
    drift = np.abs(a - c)
    damage = np.maximum(np.abs(a - b), np.abs(c - b))
    height, width, _ = damage.shape
    tiles = damage.reshape(height // 8, 8, width // 8, 8, 3).mean(axis=(1, 3, 4))
    return {"reference_mean": float(drift.mean()), "reference_peak": float(drift.max()),
            "mean": float(damage.mean()), "peak": float(damage.max()), "tile": float(tiles.max()),
            "pass": bool(drift.mean() <= .0005 and drift.max() <= .004 and
                         damage.mean() <= .002 and damage.max() <= .04 and tiles.max() <= .008)}


def timing(path):
    j = json.loads(path.read_text())
    if j["width"] != 1920 or j["height"] != 1080 or j["frequency"] <= 0 or len(j["frames"]) < 11:
        raise ValueError("Invalid frame evidence")
    count = len(j["frames"][0]["ticks"])
    if count not in (5, 6):
        raise ValueError("Unknown pass layout")
    walls, gpu, passes = [], [], [[] for _ in range(count - 1)]
    for frame in j["frames"]:
        ticks = frame["ticks"]
        if len(ticks) != count or any(y < x for x, y in zip(ticks, ticks[1:])):
            raise ValueError("Invalid query ordering")
        wall = frame["wall_ms"]
        if not math.isfinite(wall) or wall <= 0:
            raise ValueError("Invalid wall frame time")
        walls.append(wall)
        gpu.append((ticks[-1] - ticks[0]) * 1000 / j["frequency"])
        for i in range(count - 1):
            passes[i].append((ticks[i + 1] - ticks[i]) * 1000 / j["frequency"])
    return j, {"wall_p50_ms": stats.median(walls), "wall_p95_ms": float(np.percentile(walls, 95)),
               "gpu_p50_ms": stats.median(gpu), "pass_p50_ms": [stats.median(x) for x in passes]}, walls


def validate(root):
    trial = json.loads((root / "trial.json").read_text())
    scene = trial["scenario"]
    selected_probe = trial.get("selected_probe", 1)
    selected_mip = trial.get("selected_mip", 2)
    selected_action = trial.get("selected_action", selected_mip)
    quality = compare(*(image(root / f"probe-{selected_probe}-{i}.rgb8") for i in range(3)))
    for field in ("mean", "peak", "tile"):
        if abs(quality[field] - trial[f"image_{field}"]) > 1e-7:
            raise ValueError("Core/independent critic mismatch")
    probes = [timing(root / f"probe-{selected_probe}-{i}.json")[1] for i in range(3)]
    before, changed, after = [p["gpu_p50_ms"] for p in probes]
    stable_probe = abs(before - after) / min(before, after) <= .10
    benefit = min(before, after) - changed
    expected = 2 if quality["reference_mean"] > .0005 or quality["reference_peak"] > .004 else 3 if not quality["pass"] else 5 if not stable_probe else 6 if benefit < .02 or benefit / min(before, after) < .02 else 0
    if trial["reason"] != expected or trial["status"] != (5 if expected == 0 else 4) or not trial["restored"]:
        raise ValueError("Trial decision or restoration inconsistent with evidence")
    rounds, baseline, modified = [], [], []
    settings = set()
    for round_index in range(3):
        arms = []
        for arm in range(4):
            j, metrics, samples = timing(root / f"round-{round_index}-arm-{arm}.json")
            is_modified = arm in (0, 3) if round_index % 2 else arm in (1, 2)
            expected_mip = selected_mip if is_modified else 0
            if (j["isolated"] or j["mip"] != expected_mip or len(samples) != (61 if scene == 5 else 21) or
                j.get("action_mask", j["mip"]) != (selected_action if is_modified else 0)):
                raise ValueError("Incorrect counterbalance/presentation evidence")
            if scene == 5 and [f["camera_tick"] for f in j["frames"]] != list(range(61)):
                raise ValueError("Dynamic arm did not replay the complete identical trajectory")
            settings.add((j["material_samples"], j["light_steps"], j.get("shadow_rays", 0)))
            (modified if is_modified else baseline).extend(samples)
            arms.append({"mip": expected_mip, "modified": is_modified, **metrics})
        off = [a["wall_p50_ms"] for a in arms if not a["modified"]]
        on = [a["wall_p50_ms"] for a in arms if a["modified"]]
        rounds.append({"arms": arms, "conservative_speedup": min(off) / max(on),
                       "baseline_stable": abs(off[0] - off[1]) / min(off) <= .10})
    if len(settings) != 1:
        raise ValueError("Workload changed between measured arms")
    presented_quality = compare(*(image(root / f"frame-arm-{i}.rgb8") for i in (0, 1, 3)))
    off, on = stats.median(baseline), stats.median(modified)
    x2 = (28 <= off <= 38 and on <= 1000 / 60 and
          all(r["baseline_stable"] and r["conservative_speedup"] >= 2 for r in rounds) and
          trial["status"] == 5 and quality["pass"] and presented_quality["pass"])
    result = {"schema": 1, "scenario": scene, "x2_goal": "PASS" if x2 else "FAIL",
              "scope": "owned synthetic 1080p full-frame workload; serialized render/Present throughput, not game or display FPS",
              "wall_baseline_ms": off, "wall_modified_ms": on, "baseline_throughput_fps": 1000 / off,
              "modified_throughput_fps": 1000 / on, "speedup": off / on,
              "probe_cost_ms": trial["probe_wall_ms"],
              "probe_amortization_frames": math.ceil(trial["probe_wall_ms"] / (off - on)) if off > on else None,
              "quality": quality, "presented_quality": presented_quality, "trial": trial, "rounds": rounds}
    candidates_path = root / "candidates.json"
    if "selected_action" in trial and candidates_path.exists():
        candidates = json.loads(candidates_path.read_text())
        result["candidates"] = []
        for candidate in candidates:
            probe = candidate["probe"]
            q = compare(*(image(root / f"probe-{probe}-{i}.rgb8") for i in range(3)))
            captures = [timing(root / f"probe-{probe}-{i}.json") for i in range(3)]
            if scene == 5:
                for capture, _, _ in captures:
                    if [f["camera_tick"] for f in capture["frames"]] != list(range(61)):
                        raise ValueError("Dynamic reference/probe trajectories differ")
            if any((j["material_samples"], j["light_steps"], j["shadow_rays"]) not in settings for j, _, _ in captures):
                raise ValueError("Probe used a different workload")
            if [j["action_mask"] for j, _, _ in captures] != [0, candidate["action_mask"], 0]:
                raise ValueError("Probe did not restore all domains")
            ta, tb, tc = [m["gpu_p50_ms"] for _, m, _ in captures]
            stable = abs(ta - tc) / min(ta, tc) <= .10
            gain = min(ta, tc) - tb
            reason = 2 if q["reference_mean"] > .0005 or q["reference_peak"] > .004 else 3 if not q["pass"] else 5 if not stable else 6 if gain < .02 or gain / min(ta, tc) < .02 else 0
            if candidate["reason"] != reason or candidate["status"] != (5 if reason == 0 else 4):
                raise ValueError("Independent candidate verdict mismatch")
            if reason in (0, 6) and abs(gain - candidate["gain_ms"]) > 1e-7:
                raise ValueError("Reported candidate gain differs from raw timestamps")
            result["candidates"].append({**candidate, "quality": q, "independent_gain_ms": gain})
        accepted = [c for c in candidates if c["status"] == 5]
        accepted = [c for c in result["candidates"] if c["status"] == 5]
        best = max(accepted, key=lambda c: c["independent_gain_ms"]) if accepted else candidates[0]
        if best["probe"] != selected_probe:
            raise ValueError("Selected action inconsistent with validated gains")
        if scene == 2:
            result["detail_protection"] = "PASS" if all(c["reason"] == 3 for c in candidates if c["action_mask"] in (2, 3)) else "FAIL"
    elif scene == 2:
        result["negative_control"] = "PASS" if expected == 3 and not presented_quality["pass"] else "FAIL"
    elif scene == 1:
        result["negative_control"] = "PASS" if not x2 and off / on < 2 else "FAIL"
    (root / "independent-acceptance.json").write_text(json.dumps(result, indent=2))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directories", type=Path, nargs="+")
    args = parser.parse_args()
    for directory in args.directories:
        result = validate(directory)
        print(json.dumps({k: v for k, v in result.items() if k not in ("rounds", "trial")}, indent=2))
