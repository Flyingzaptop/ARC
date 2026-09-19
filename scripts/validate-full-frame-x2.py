"""Full-frame capacity experiment: independently recompute images and GPU ticks."""
import argparse
import json
import math
import statistics as stats
from pathlib import Path
import numpy as np


def image(path):
    raw = np.fromfile(path, dtype=np.uint8)
    if raw.size != 1920 * 1080 * 3:
        raise ValueError(f"Incomplete 1080p RGB image: {path}")
    # Match the core's normalized float32 RGB contract, then compute in float64.
    return (raw.astype(np.float32) / np.float32(255)).astype(np.float64).reshape(1080, 1920, 3)


def compare(a, b, c):
    drift = np.abs(a - c)
    damage = np.maximum(np.abs(a - b), np.abs(c - b))
    tiles = damage.reshape(135, 8, 240, 8, 3).mean(axis=(1, 3, 4))
    return {"reference_mean": float(drift.mean()), "reference_peak": float(drift.max()),
            "mean": float(damage.mean()), "peak": float(damage.max()), "tile": float(tiles.max()),
            "pass": bool(drift.mean() <= .0005 and drift.max() <= .004 and
                         damage.mean() <= .002 and damage.max() <= .04 and tiles.max() <= .008)}


def timing(path):
    j = json.loads(path.read_text())
    if j["width"] != 1920 or j["height"] != 1080 or j["frequency"] <= 0 or len(j["frames"]) < 11:
        raise ValueError("Invalid frame evidence")
    walls, gpu, passes = [], [], [[], [], [], []]
    for frame in j["frames"]:
        ticks = frame["ticks"]
        if len(ticks) != 5 or any(y < x for x, y in zip(ticks, ticks[1:])):
            raise ValueError("Invalid query ordering")
        wall = frame["wall_ms"]
        if not math.isfinite(wall) or wall <= 0:
            raise ValueError("Invalid wall frame time")
        walls.append(wall)
        gpu.append((ticks[-1] - ticks[0]) * 1000 / j["frequency"])
        for i in range(4):
            passes[i].append((ticks[i + 1] - ticks[i]) * 1000 / j["frequency"])
    return j, {"wall_p50_ms": stats.median(walls), "wall_p95_ms": float(np.percentile(walls, 95)),
               "gpu_p50_ms": stats.median(gpu), "pass_p50_ms": [stats.median(x) for x in passes]}, walls


def validate(root):
    trial = json.loads((root / "trial.json").read_text())
    scene = trial["scenario"]
    quality = compare(*(image(root / f"probe-1-{i}.rgb8") for i in range(3)))
    for field in ("mean", "peak", "tile"):
        if abs(quality[field] - trial[f"image_{field}"]) > 1e-7:
            raise ValueError("Core/independent critic mismatch")
    probes = [timing(root / f"probe-1-{i}.json")[1] for i in range(3)]
    before, changed, after = [p["gpu_p50_ms"] for p in probes]
    stable_probe = abs(before - after) / min(before, after) <= .10
    benefit = min(before, after) - changed
    expected = 3 if not quality["pass"] else 5 if not stable_probe else 6 if benefit < .02 or benefit / min(before, after) < .02 else 0
    if trial["reason"] != expected or trial["status"] != (5 if expected == 0 else 4) or not trial["restored"]:
        raise ValueError("Trial decision or restoration inconsistent with evidence")
    rounds, baseline, modified = [], [], []
    settings = set()
    for round_index in range(3):
        arms = []
        for arm in range(4):
            j, metrics, samples = timing(root / f"round-{round_index}-arm-{arm}.json")
            expected_mip = 2 if (arm in (0, 3) if round_index % 2 else arm in (1, 2)) else 0
            if j["isolated"] or j["mip"] != expected_mip or len(samples) != 21:
                raise ValueError("Incorrect counterbalance/presentation evidence")
            settings.add((j["material_samples"], j["light_steps"]))
            (modified if expected_mip else baseline).extend(samples)
            arms.append({"mip": expected_mip, **metrics})
        off = [a["wall_p50_ms"] for a in arms if a["mip"] == 0]
        on = [a["wall_p50_ms"] for a in arms if a["mip"] == 2]
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
    if scene == 2:
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
