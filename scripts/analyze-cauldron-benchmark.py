"""Validate matched test-host runs; retain raw tails and failed image checks."""
import argparse
import csv
import io
import json
from pathlib import Path
import numpy as np
from PIL import Image
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

parser=argparse.ArgumentParser();parser.add_argument("directory",type=Path)
root=parser.parse_args().directory.resolve()
names=["baseline-a","v01","current","baseline-b"]
runs={};manifests={}
for name in names:
    path=root/name
    manifest=json.loads((path/"manifest.json").read_text());manifests[name]=manifest
    vendor_files=list(path.glob("*-perf-*.json"))
    if len(vendor_files)!=1 or json.loads(vendor_files[0].read_text())["RenderResolution"]!=[1920,1080]:raise ValueError("Non-native render resolution")
    rows=[json.loads(line) for line in (path/"frames.jsonl").read_text().splitlines()]
    if len(rows)!=600 or [r["frame"] for r in rows]!=list(range(600)) or sorted(r["pose"] for r in rows)!=list(range(600)):
        raise ValueError(f"Incomplete or unmatched trajectory: {name}")
    if any("FPSLimiter" in r["gpu_ms"] or "FPSLimiter" in r["cpu_ms"] for r in rows):
        raise ValueError("Artificial FPS limiter contaminated the test")
    periods=np.array([r["frame_ms"] for r in rows]);worst=np.sort(periods)[-6:]
    if not np.isfinite(periods).all() or np.any(periods<=0):raise ValueError("Invalid cadence")
    result={"fps":float(1000/periods.mean()),"frame_ms":float(periods.mean()),
            "one_percent_low_fps":float(1000/worst.mean()),"p99_ms":float(np.quantile(periods,.99)),
            "cpu_ms":{k:float(np.mean([r["cpu_ms"].get(k,0) for r in rows])) for k in rows[0]["cpu_ms"]},
            "gpu_ms":{k:float(np.mean([r["gpu_ms"].get(k,0) for r in rows])) for k in rows[0]["gpu_ms"]},
            "gpu_span_ms":float(np.mean([r["gpu_span_ms"] for r in rows])),
            "submit_ms":float(np.mean([r["submit_ms"] for r in rows])),
            "swapchain_wait_ms":float(np.mean([r["swapchain_wait_ms"] for r in rows]))}
    cpu=json.loads((path/"cpu.json").read_text())
    result["process_cpu_equivalent_cores"]=cpu["process_cpu_ms"]/(cpu["seconds"]*1000)
    result["busiest_thread_one_core_percent"]=max(t["percent_of_one_logical_cpu"] for t in cpu["threads"])
    hardware=[]
    for sample in json.loads((path/"gpu-samples.json").read_text()):
        if sample["measured_phase"] and sample["exit_code"]==0:
            for row in csv.DictReader(io.StringIO(sample["csv"])):
                hardware.append({k.strip():v.strip() for k,v in row.items()})
    result["hardware_sample_count"]=len(hardware)
    result["hardware_mean"]={k:float(np.mean([float(row[k]) for row in hardware])) for k in hardware[0] if k!="timestamp"} if hardware else {}
    if (path/"arc.json").exists():
        arc=json.loads((path/"arc.json").read_text());control=arc["command_mirror"]
        if arc["present_failures"] or control["faults"] or not control["modified_draws"]:raise ValueError("ARC did not execute a healthy trial")
        if control["requested_rate"]!=0:raise ValueError("Trial did not request rollback")
        result["arc"]=control
    runs[name]=result
if len({m["host_sha256"] for m in manifests.values()})!=1:raise ValueError("Different host binaries")
if manifests["baseline-a"]["dll_sha256"] or manifests["baseline-b"]["dll_sha256"]:raise ValueError("Injected baseline")

def pixels(path):
    files=list(path.glob("*.png"))
    if len(files)!=1:raise ValueError(f"Expected one final pose image: {path}")
    return np.asarray(Image.open(files[0]).convert("RGB"),dtype=np.float32)/255

def quality(reference,candidate):
    # Verified in Cauldron's transferFunction.h: ApplyGamma = pow(linear, 1/2.2).
    diff=np.abs(reference**2.2-candidate**2.2)
    return {"mean_linear_error":float(diff.mean()),"peak_linear_error":float(diff.max()),
            "worst_8x8_tile_error":float(diff.reshape(135,8,240,8,3).mean((1,3,4)).max())}

reference=pixels(root/"baseline-a");repeat=pixels(root/"baseline-b")
reference_error=quality(reference,repeat)
checks=[{"pose":119,**quality(reference,pixels(root/"current"))}]
previews=[Image.fromarray((reference*255).astype(np.uint8))]
for frames in (151,301,451):
    pair=root/"quality"/f"frames-{frames}"
    a=pixels(pair/"baseline");b=pixels(pair/"current")
    checks.append({"pose":(120+frames-1)%600,**quality(a,b)})
    previews.append(Image.fromarray((a*255).astype(np.uint8)))
for check in checks:
    check["within_image_limits"]=(check["mean_linear_error"]<=.002 and check["peak_linear_error"]<=.04 and check["worst_8x8_tile_error"]<=.008)
baseline_frame=(runs["baseline-a"]["frame_ms"]+runs["baseline-b"]["frame_ms"])/2
summary={"schema":1,"scene":"AMD Toyshop/Teddy, Cauldron Brixelizer GI 1.1.4",
         "native_resolution":[1920,1080],"upscaling":False,"frame_generation":False,
         "runs":runs,"baseline_combined_fps":1000/baseline_frame,
         "baseline_repeat_drift_fraction":abs(runs["baseline-a"]["fps"]-runs["baseline-b"]["fps"])/min(runs["baseline-a"]["fps"],runs["baseline-b"]["fps"]),
         "current_fps_ratio":runs["current"]["fps"]/(1000/baseline_frame),
         "cpu_record_reduction_vs_v01":1-runs["current"]["cpu_ms"]["RM Executes"]/runs["v01"]["cpu_ms"]["RM Executes"],
         "reference_repeat_error":reference_error,"quality_checks":checks,
         "reference_within_limits":reference_error["mean_linear_error"]<=.0005 and reference_error["peak_linear_error"]<=.004,
         "image_guard_pass":all(c["within_image_limits"] for c in checks) and reference_error["mean_linear_error"]<=.0005 and reference_error["peak_linear_error"]<=.004,
         "universal_or_2x_claim":False,"manifests":manifests}
(root/"analysis.json").write_text(json.dumps(summary,indent=2))
plt.rcParams.update({"font.family":"DejaVu Sans","font.size":10})
fig,axes=plt.subplots(1,2,figsize=(12,4.8),layout="constrained")
labels=["Без ARC (A)","Старая v0.1","Исправленный ARC","Без ARC (B)"]
axes[0].bar(labels,[runs[n]["fps"] for n in names],color=["#7e929f","#cb816c","#55a6a0","#7e929f"])
axes[0].set_ylabel("FPS");axes[0].set_title("Нативные 1920×1080, динамическая сцена");axes[0].tick_params(axis="x",labelrotation=15)
keys=["RasterShadow","GBuffer","Brixelizer GI","TAA"]
y=np.arange(len(keys));base=[(runs['baseline-a']['gpu_ms'][k]+runs['baseline-b']['gpu_ms'][k])/2 for k in keys]
axes[1].barh(y-.17,base,.32,label="Без ARC",color="#7e929f")
axes[1].barh(y+.17,[runs['current']['gpu_ms'][k] for k in keys],.32,label="Исправленный ARC",color="#55a6a0")
axes[1].set_yticks(y,["Тени","G-buffer","GI + освещение","TAA"]);axes[1].set_xlabel("GPU, мс");axes[1].set_title("GI включает вложенные проходы");axes[1].legend()
fig.savefig(root/"profile.png",dpi=160);plt.close(fig)
small=[im.resize((768,432),Image.Resampling.LANCZOS) for im in previews]
small[0].save(root/"scene-preview.gif",save_all=True,append_images=small[1:],duration=500,loop=0)
fig,axes=plt.subplots(1,3,figsize=(15,3.5),layout="constrained")
candidate=pixels(root/'current');axes[0].imshow(reference);axes[0].set_title('Без ARC');axes[1].imshow(candidate);axes[1].set_title('Экспериментальный VRS');axes[2].imshow(np.clip(abs(reference-candidate)*8,0,1));axes[2].set_title('Разница ×8 — проверка качества не пройдена')
for ax in axes:ax.axis('off')
fig.savefig(root/'quality-comparison.png',dpi=150);plt.close(fig)
print(json.dumps({k:summary[k] for k in ['baseline_combined_fps','current_fps_ratio','baseline_repeat_drift_fraction','cpu_record_reduction_vs_v01','image_guard_pass']},indent=2))
