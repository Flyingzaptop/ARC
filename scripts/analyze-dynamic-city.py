"""Compare matched native scene trajectories, timings and independent images."""
import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load_run(path):
    meta = json.loads((path / "run.json").read_text())
    rows = list(csv.DictReader((path / "frames.csv").open()))
    assert len(rows) == meta["measured_frames"]
    assert np.isfinite(meta["fps"]) and meta["fps"] > 0 and meta["seconds"] > 0
    assert abs(meta["fps"] - len(rows)/meta["seconds"]) < 1e-6
    sequences = [int(r["frame"]) for r in rows]
    assert sequences == list(range(sequences[0], sequences[0]+len(rows)))
    ticks = Counter(int(r["tick"]) for r in rows)
    assert set(ticks) == set(range(meta["poses_per_cycle"])) and len(set(ticks.values())) == 1
    fields = ["cpu_frame_ms", "record_ms", "submit_ms", "present_ms", "fence_wait_ms",
              "timing_readback_ms", "gpu_ms", "shadow_ms", "geometry_ms", "fog_ms", "composite_ms"]
    data = {k: np.array([float(r[k]) for r in rows]) for k in fields}
    assert all(np.isfinite(a).all() and (a >= 0).all() for a in data.values())
    assert np.max(np.abs(data["gpu_ms"] - sum(data[k] for k in fields[-4:]))) < 1e-6
    begin = [int(r["gpu_begin_tick"]) for r in rows]
    end = [int(r["gpu_end_tick"]) for r in rows]
    gaps = np.array([begin[i] - end[i-1] for i in range(1, len(rows))], dtype=np.float64)
    assert (gaps >= 0).all(), "GPU execution ordering changed"
    gaps *= 1000.0 / meta["gpu_timestamp_frequency"]
    worst = np.sort(data["cpu_frame_ms"])[-max(1, (len(rows)+99)//100):]
    poses = {}
    for r in rows:
        tick = int(r["tick"])
        pose = tuple(float(r[k]) for k in ("camera_x", "camera_y", "camera_z"))
        assert poses.setdefault(tick, pose) == pose, "Pose changed between replay cycles"
    result = {"fps": meta["fps"], "frame_ms": 1000/meta["fps"], "frames": len(rows),
              "seconds": meta["seconds"], "cycles": len(rows)//meta["poses_per_cycle"],
              "frame_p99_ms": float(np.quantile(data["cpu_frame_ms"], .99)),
              "one_percent_low_fps": float(1000/np.mean(worst)),
              "mean_ms": {k: float(a.mean()) for k, a in data.items()},
              "gpu_interframe_gap_ms": float(gaps.mean())}
    return meta, poses, result


def main():
    if not __debug__:
        raise SystemExit("Validation requires Python assertions enabled; do not use -O")
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    root = args.directory.resolve()
    runs = {name: load_run(root/name) for name in ("baseline-a", "v01", "baseline-b")}
    keys = ("scene", "adapter", "width", "height", "instances", "shadow_casters", "lights",
            "fog_steps", "shadow_size", "poses_per_cycle", "scene_cycle_seconds", "vsync", "frames_in_flight")
    a, b, c = [runs[name] for name in runs]
    assert all(a[0][k] == b[0][k] == c[0][k] for k in keys)
    assert a[1] == b[1] == c[1], "Camera trajectories differ"
    assert not a[0]["arc_loaded"] and not c[0]["arc_loaded"] and b[0]["arc_loaded"]
    assert all(meta["image_copies_during_measurement"] == 0 for meta, _, _ in runs.values())
    telemetry = json.loads((root/"v01"/"arc.json").read_text())
    mirror = telemetry["command_mirror"]
    assert telemetry["present_failures"] == 0 and mirror["faults"] == 0
    expected_submissions = b[0]["measured_frames"] + b[0]["warmup_frames"] + 12
    assert 0 < mirror["modified_submissions"] <= expected_submissions
    assert expected_submissions - mirror["modified_submissions"] == mirror["skipped"], "Unexplained missing substitution"
    assert mirror["modified_draws"] == 2*mirror["modified_submissions"]
    assert mirror["requested_rate"] == 0, "Experimental mode did not restore"
    quality = []
    files = sorted((root/"baseline-a").glob("frame-*.png"), key=lambda p: int(p.stem.split("-")[1]))
    assert len(files) == 12
    previews = []
    for file in files:
        images = [np.asarray(Image.open(root/name/file.name).convert("RGB"), dtype=np.float32)/255
                  for name in runs]
        assert all(im.shape == (a[0]["height"], a[0]["width"], 3) for im in images)
        # The scene's composite shader explicitly encodes pow(linear, 1/2.2).
        la, lb, lc = [im ** 2.2 for im in images]
        reference = np.abs(la-lc)
        damage = np.maximum(np.abs(la-lb), np.abs(lc-lb))
        h, w, _ = damage.shape
        tiles = damage.reshape(h//8, 8, w//8, 8, 3).mean(axis=(1, 3, 4))
        q = {"tick": int(file.stem.split("-")[1]), "reference_identical": bool(np.array_equal(images[0], images[2])),
             "reference_mean": float(reference.mean()), "reference_peak": float(reference.max()),
             "mean_linear_error": float(damage.mean()), "peak_linear_error": float(damage.max()),
             "worst_8x8_tile_error": float(tiles.max())}
        q["image_guard_pass"] = (q["reference_mean"] <= .0005 and q["reference_peak"] <= .004
                                  and q["mean_linear_error"] <= .002 and q["peak_linear_error"] <= .04
                                  and q["worst_8x8_tile_error"] <= .008)
        quality.append(q)
        previews.append(Image.open(file).convert("RGB").resize((768, 432), Image.Resampling.LANCZOS))
    previews[0].save(root/"scene-preview.gif", save_all=True, append_images=previews[1:], duration=250, loop=0)
    baseline_frame = (a[2]["frame_ms"] + c[2]["frame_ms"])/2
    baseline_fps = 1000/baseline_frame
    mean = {k: (a[2]["mean_ms"][k]+c[2]["mean_ms"][k])/2 for k in a[2]["mean_ms"]}
    drift = abs(a[2]["fps"]-c[2]["fps"])/min(a[2]["fps"], c[2]["fps"])
    summary = {"schema": 1, "runs": {name: item[2] for name, item in runs.items()},
               "baseline_combined_fps": baseline_fps, "fps_ratio": b[2]["fps"]/baseline_fps,
               "baseline_repeat_drift_fraction": drift, "poses_match": True,
               "image_guard_all_pass": all(q["image_guard_pass"] for q in quality),
               "image_guard_thresholds": {"mean_linear": .002, "peak_linear": .04, "tile_8x8": .008},
               "image_checks": quality, "full_game_or_2x_claim": False,
               "vrs_application": {"scope": "including_warmup_and_image_captures", "expected_submissions": expected_submissions,
                                   "modified_submissions": mirror["modified_submissions"], "fallbacks": mirror["skipped"],
                                   "coverage_fraction": mirror["modified_submissions"]/expected_submissions},
               "files": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.glob("*.exe")}}
    dll = root/"arc-dx12-probe.dll"
    summary["files"][dll.name] = hashlib.sha256(dll.read_bytes()).hexdigest()
    diagnostics = {}
    for name in ("offscreen-baseline", "offscreen-v01"):
        if (root/name/"run.json").exists():
            meta, poses, stats = load_run(root/name)
            assert meta["presentation_enabled"] is False and poses == a[1]
            reference = "baseline-a" if name.endswith("baseline") else "v01"
            matching = sum(np.array_equal(np.asarray(Image.open(file)), np.asarray(Image.open(root/name/file.name)))
                           for file in (root/reference).glob("frame-*.png"))
            diagnostics[name] = {**stats, "matching_images": matching, "image_count": 12}
    summary["offscreen_diagnostics"] = diagnostics
    (root/"analysis.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10})
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.4), constrained_layout=True)
    categories = ["Карта теней", "Геометрия + свет", "Объёмный свет (compute)", "Тонмаппинг + bloom"]
    fields = ["shadow_ms", "geometry_ms", "fog_ms", "composite_ms"]
    y = np.arange(4)
    axes[0].barh(y-.17, [mean[k] for k in fields], .32, label="Baseline", color="#5277b8")
    axes[0].barh(y+.17, [b[2]["mean_ms"][k] for k in fields], .32, label="v0.1", color="#dc598a")
    axes[0].set_yticks(y, categories); axes[0].invert_yaxis(); axes[0].set_xlabel("Среднее GPU-время, мс");axes[0].legend()
    cpu_fields = ["record_ms", "submit_ms", "timing_readback_ms", "present_ms"]
    axes[1].barh(y-.17, [mean[k] for k in cpu_fields], .32, color="#5277b8")
    axes[1].barh(y+.17, [b[2]["mean_ms"][k] for k in cpu_fields], .32, color="#dc598a")
    axes[1].set_yticks(y, ["Подготовка команд", "Отправка команд", "Чтение таймеров", "Present (включая ожидание)"])
    axes[1].invert_yaxis();axes[1].set_xlabel("Среднее CPU-время вызовов, мс")
    for ax in axes: ax.grid(axis="x", alpha=.2);ax.set_axisbelow(True)
    fig.suptitle(f"Dynamic City · 1920×1080 · {baseline_fps:.1f} → {b[2]['fps']:.1f} FPS ({(summary['fps_ratio']-1)*100:+.1f}%)")
    fig.savefig(root/"profile.png", dpi=150);plt.close(fig)

    lines = ["# Dynamic City: baseline vs current v0.1", "", "Identical closed camera route, actors and settings; clean processes without ARC for both baselines. No full-frame readback during timing. The v0.1 run uses the actual generic DX12 VRS command-substitution DLL in lean mode.", "",
             "| Run | Frames | Seconds | FPS | p99 frame ms | 1% low FPS |", "|---|---:|---:|---:|---:|---:|"]
    for name, (_, _, r) in runs.items(): lines.append(f"| {name} | {r['frames']} | {r['seconds']:.3f} | {r['fps']:.2f} | {r['frame_p99_ms']:.3f} | {r['one_percent_low_fps']:.2f} |")
    lines += ["", f"Combined baseline: **{baseline_fps:.2f} FPS**; v0.1: **{b[2]['fps']:.2f} FPS**; change **{(summary['fps_ratio']-1)*100:+.2f}%**. Baseline repeat drift: {drift*100:.2f}%.", "",
              f"Actual substitution coverage including warmup/captures: {mirror['modified_submissions']}/{expected_submissions}; {mirror['skipped']} bounded-capacity fallbacks used the original command list. These fallbacks remain included in the measured result.", "",
              "| Component | Baseline ms | v0.1 ms |", "|---|---:|---:|"]
    for name, key in zip(categories+["Total measured GPU", "CPU recording", "CPU submission", "GPU fence wait", "Timing readback CPU", "CPU Present"], fields+["gpu_ms", "record_ms", "submit_ms", "fence_wait_ms", "timing_readback_ms", "present_ms"]):
        lines.append(f"| {name} | {mean[key]:.4f} | {b[2]['mean_ms'][key]:.4f} |")
    gap=(a[2]["gpu_interframe_gap_ms"]+c[2]["gpu_interframe_gap_ms"])/2
    lines += ["", f"Inter-frame GPU-timestamp gap: {gap:.4f} → {b[2]['gpu_interframe_gap_ms']:.4f} ms. This includes queue gaps and work outside the marked render passes; it is not identified solely as CPU or driver time.", "",
              f"Fog/volumetric compute contributes {b[2]['mean_ms']['fog_ms']/b[2]['mean_ms']['gpu_ms']*100:.1f}% of the remaining marked GPU work. VRS acts on raster pixel shading, so this compute pass is unchanged in mechanism.", "",
              f"Image guard: **{'PASS' if summary['image_guard_all_pass'] else 'FAIL'}** across 12 matched poses. Full-quality references identical: {sum(q['reference_identical'] for q in quality)}/12. Worst mean/peak/tile linear error: {max(q['mean_linear_error'] for q in quality):.6f} / {max(q['peak_linear_error'] for q in quality):.6f} / {max(q['worst_8x8_tile_error'] for q in quality):.6f}.", "",
              "This is a lightweight instanced synthetic scene, not a game-speedup result. Geometry consists of 1,294 procedural cuboid instances; there is no DXR, streaming, game simulation or engine overhead. The camera follows a fixed 15-second simulation cycle, replayed without a wall-time FPS cap. All complete cycles have the same pose distribution. The preview GIF is a sparse accelerated route overview.", "",
              "Next: address the measured compute-lighting cost; use conservative quality-aware controls and retain independent reference/rollback checks. Reduce command-substitution overhead and preserve fine edges before accepting a whole-frame VRS change. Add meaningful geometry/material/streaming complexity without inflating baseline work merely to manufacture a speedup.", ""]
    if len(diagnostics) == 2:
        da, db = diagnostics["offscreen-baseline"], diagnostics["offscreen-v01"]
        lines += ["## Presentation-path diagnostic", "",
                  f"Using three ordinary offscreen render targets instead of the presentation path: {da['fps']:.2f} → {db['fps']:.2f} rendering FPS ({(db['fps']/da['fps']-1)*100:+.1f}%). These are throughput diagnostics and are not the main displayed-window comparison.", "",
                  f"The inter-frame GPU gap drops to {da['gpu_interframe_gap_ms']:.4f} / {db['gpu_interframe_gap_ms']:.4f} ms. Corresponding screenshots match {da['matching_images']}/12 baseline and {db['matching_images']}/12 v0.1 images. Thus the presentation path is a material limiter in this windowed benchmark, in addition to volumetric compute. The diagnostic does not isolate the OS compositor, driver and API costs from one another.", ""]
    (root/"REPORT.md").write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({"baseline_fps": baseline_fps, "v01_fps": b[2]["fps"], "ratio": summary["fps_ratio"], "baseline_drift": drift,
                      "image_guard": summary["image_guard_all_pass"], "reference_identical": sum(q["reference_identical"] for q in quality),
                      "worst_image_errors": {k:max(q[k] for q in quality) for k in ("mean_linear_error", "peak_linear_error", "worst_8x8_tile_error")}}, indent=2))


if __name__ == "__main__":
    main()
