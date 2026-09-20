"""Run the owned Cauldron test host, with a 60 second process budget."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import shutil
from benchmark_thermal_gate import wait_for_cool_gpu

parser=argparse.ArgumentParser()
parser.add_argument("sdk",type=Path)
parser.add_argument("output",type=Path)
parser.add_argument("--dll",type=Path)
parser.add_argument("--mode",choices=["vrs","observe","profile","compute-neutral","compute-2x1","compute-1x2","compute-2x2","compute-pcf9","compute-zero","compute-neutral-hot","compute-2x1-hot","compute-1x2-hot","compute-2x2-hot","compute-pcf9-hot","compute-zero-hot","compute-adaptive-1x2-hot","compute-adaptive-2x2-hot","compute-mip-half-hot","compute-mip1-hot","compute-mip2-hot"],default="vrs")
parser.add_argument("--frames",type=int,default=600)
parser.add_argument("--process-sampler",type=Path)
parser.add_argument("--edge-threshold",type=float)
parser.add_argument("--auto-target",type=float)
parser.add_argument("--compiler",type=Path)
parser.add_argument("--measure-costs",action="store_true")
parser.add_argument("--worker-placement",choices=['normal','prefer','core','partition','adaptive'],default='normal')
parser.add_argument("--max-start-temperature",type=float)
args=parser.parse_args()
sdk=args.sdk.resolve();output=args.output.resolve()
output.mkdir(parents=True,exist_ok=False)
exe=sdk/"bin/FFX_BRIXELIZER_GI_DX12.exe"
env=os.environ.copy()
for key in list(env):
    if key.startswith('ARC_WORKER_'): env.pop(key)
if args.worker_placement!='normal' and not args.dll: raise ValueError('Worker placement requires ARC DLL')
env['ARC_WORKER_PLACEMENT']=args.worker_placement
if args.worker_placement=='partition': env['ARC_WORKER_ALLOW_PARTITION']='1'
env["ARC_BENCH_OUTPUT"]=str(output)
env["ARC_BENCH_FRAMES"]=str(args.frames)
env.pop("ARC_BENCH_DLL",None)
env["ARC_BENCH_MODE"]=args.mode
if args.edge_threshold is not None:
    if not args.mode.startswith("compute-adaptive-") or not 0 <= args.edge_threshold <= 2:
        raise ValueError("Edge threshold 0..2 requires an adaptive compute mode")
    env["ARC_BENCH_MODE"]=args.mode.replace("-hot", f"@{args.edge_threshold}-hot")
if args.dll: env["ARC_BENCH_DLL"]=str(args.dll.resolve())
for key in ("ARC_OPTIMIZER_WORKER","ARC_OPTIMIZER_COMPILER","ARC_OPTIMIZER_CACHE"):
    env.pop(key,None)
for key in ("ARC_OPTIMIZER_CPU_TIMING","ARC_OPTIMIZER_GPU_CONTROL_TIMING"):
    env.pop(key,None)
    if args.measure_costs and args.dll: env[key]="1"
if args.dll and args.mode.startswith("compute-"):
    worker=Path(__file__).resolve().parents[1]/"build/Release/arc-shader-tool.exe"
    compiler=args.compiler.resolve() if args.compiler else sdk/"framework/cauldron/framework/libs/dxc/bin/x64/dxcompiler.dll"
    if not worker.is_file() or not compiler.is_file():
        raise RuntimeError("Compute experiment requires built ARC shader worker and pinned DXC")
    env["ARC_OPTIMIZER_WORKER"]=str(worker)
    env["ARC_OPTIMIZER_COMPILER"]=str(compiler)
    env["ARC_OPTIMIZER_CACHE"]=str(output/"shader-cache")
env.pop("ARC_AUTO_CONFIG",None)
if args.auto_target is not None:
    if not args.dll or not args.mode.startswith("compute-") or not 0 < args.auto_target <= 1000:
        raise ValueError("Automatic target requires a compute-enabled DLL and FPS in (0,1000]")
    import sys
    config=output/"automatic-config.json"
    config.write_text(json.dumps({"target_fps":args.auto_target,"python":sys.executable,
        "critic":str(Path(__file__).resolve().parent/"optimizer-live-quality.py"),
        "output":str(output/"automatic"),"maximum_seconds":50},indent=2))
    env["ARC_AUTO_CONFIG"]=str(config)
command=[str(exe),"-resolution","1920","1080","-benchmark",f"duration={args.frames+120}",f"path={output}","json","-screenshot"]
hashfile=lambda p:hashlib.file_digest(p.open("rb"),"sha256").hexdigest()
manifest={"host_sha256":hashfile(exe),"dll_sha256":hashfile(args.dll.resolve()) if args.dll else None,
          "mode":args.mode if args.dll else "baseline","command":command,"measured_frames":args.frames,"warmup_frames":120,
          "edge_threshold":args.edge_threshold,"effective_mode":env["ARC_BENCH_MODE"],
          "automatic_target_fps":args.auto_target,
          "cost_diagnostics":bool(args.measure_costs and args.dll),
          "worker_placement":args.worker_placement,
          "compiler_sha256":hashfile(compiler) if args.dll and args.mode.startswith("compute-") else None,
          "simulation_dt":1/60,"camera_period_frames":600,"vsync":False,
          "fps_limiter":False,"upscaling":False,"frame_generation":False}
(output/"manifest.json").write_text(json.dumps(manifest,indent=2))
manifest['thermal_gate']=wait_for_cool_gpu(args.max_start_temperature)
(output/"manifest.json").write_text(json.dumps(manifest,indent=2))
startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
started=time.monotonic()
cpu=None;gpu_rows=[];next_gpu=0
with (output/"stdout.txt").open("w") as stdout,(output/"stderr.txt").open("w") as stderr:
    process=subprocess.Popen(command,cwd=exe.parent,env=env,stdout=stdout,stderr=stderr,startupinfo=startup)
    try:
        while process.poll() is None:
            if time.monotonic()-started>60: raise subprocess.TimeoutExpired(command,60)
            rows_path=output/"frames.jsonl"
            if args.frames>=600 and shutil.which("nvidia-smi") and time.monotonic()>=next_gpu:
                sample=subprocess.run(["nvidia-smi","--query-gpu=timestamp,utilization.gpu,utilization.memory,clocks.current.graphics,memory.used,temperature.gpu,power.draw","--format=csv,nounits"],capture_output=True,text=True,timeout=3,creationflags=subprocess.CREATE_NO_WINDOW)
                gpu_rows.append({"elapsed_s":time.monotonic()-started,"measured_phase":rows_path.exists() and rows_path.stat().st_size>0,"csv":sample.stdout,"exit_code":sample.returncode})
                next_gpu=time.monotonic()+1
            if cpu is None and args.process_sampler and args.frames>=600 and rows_path.exists() and rows_path.stat().st_size:
                cpu=subprocess.Popen([str(args.process_sampler.resolve()),str(process.pid),"5",str(output/"cpu.json")],stdout=subprocess.DEVNULL,stderr=stderr,creationflags=subprocess.CREATE_NO_WINDOW)
            time.sleep(0.05)
        code=process.returncode
    except subprocess.TimeoutExpired:
        process.kill();process.wait();raise RuntimeError("Owned benchmark exceeded 60 seconds")
    finally:
        if cpu:
            try: cpu.wait(timeout=6)
            except subprocess.TimeoutExpired: cpu.kill();cpu.wait()
(output/"gpu-samples.json").write_text(json.dumps(gpu_rows,indent=2))
manifest["exit_code"]=code;manifest["process_seconds"]=time.monotonic()-started
(output/"manifest.json").write_text(json.dumps(manifest,indent=2))
log=exe.parent/"Cauldron.log"
if log.exists(): (output/"Cauldron.log").write_bytes(log.read_bytes())
if code: raise RuntimeError(f"Host failed: {code}")
rows=[json.loads(line) for line in (output/"frames.jsonl").read_text().splitlines()]
manifest['actual_frames']=len(rows);manifest['measurement_complete']=len(rows)==args.frames
(output/"manifest.json").write_text(json.dumps(manifest,indent=2))
if len(rows)!=args.frames: raise RuntimeError(f"Incomplete measurement: {len(rows)}/{args.frames}")
fps=1000*len(rows)/sum(row["frame_ms"] for row in rows)
print(json.dumps({"output":str(output),"frames":len(rows),"fps":fps,"process_seconds":manifest["process_seconds"]}))
