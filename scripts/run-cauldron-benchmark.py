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
parser.add_argument("--mode",choices=["vrs","observe","profile","compute-off","compute-neutral","compute-2x1","compute-1x2","compute-2x2","compute-pcf9","compute-zero","compute-neutral-hot","compute-2x1-hot","compute-1x2-hot","compute-2x2-hot","compute-pcf9-hot","compute-zero-hot","compute-adaptive-1x2-hot","compute-adaptive-2x2-hot","compute-mip-half-hot","compute-mip1-hot","compute-mip2-hot"],default="vrs")
parser.add_argument("--frames",type=int,default=600)
parser.add_argument("--initialization-seconds",type=float,default=0)
parser.add_argument("--measurement-seconds",type=float,default=0)
parser.add_argument("--oracle-poses",action="store_true",help="Independent 32-pose quality oracle at absolute frames 1200..1799; not a performance run")
parser.add_argument("--oracle-interval",type=int,default=0,help="Separate quality-oracle run; invalidates performance comparison")
parser.add_argument("--route",choices=["primary","holdout"],default="primary")
parser.add_argument("--gpu-sample-interval",type=float,default=1.,help="Zero disables periodic external sensor calls for attribution tests")
parser.add_argument("--cache",type=Path)
parser.add_argument("--overlay",action="store_true")
parser.add_argument("--functional",action="store_true",help="Interactive correctness run, excluded from FPS comparisons")
parser.add_argument("--borderless",action="store_true",help="Use the SDK borderless-fullscreen option at native display resolution")
parser.add_argument("--visible",action="store_true",help="Show the owned renderer and diagnostics without activating them")
parser.add_argument("--process-sampler",type=Path)
parser.add_argument("--edge-threshold",type=float)
parser.add_argument("--auto-target",type=float)
parser.add_argument("--control-mode",choices=["validated","target_feedback"],default="validated")
parser.add_argument("--compiler",type=Path)
parser.add_argument("--shader-worker",type=Path)
parser.add_argument("--critic",type=Path)
parser.add_argument("--quality-python",type=Path)
parser.add_argument("--quality-profile",choices=["balanced","aggressive"],default="balanced")
parser.add_argument("--measure-costs",action="store_true")
parser.add_argument("--cpu-state-cache",action="store_true")
parser.add_argument("--worker-placement",choices=['normal','prefer','core','partition','adaptive'],default='normal')
parser.add_argument("--max-start-temperature",type=float)
args=parser.parse_args()
if not 0<=args.initialization_seconds<=120 or not 0<=args.measurement_seconds<=60:
    parser.error('Initialization must be 0..120s and measurement 0..60s')
if args.initialization_seconds and not args.measurement_seconds:
    parser.error('Timed initialization requires a timed measurement')
if args.oracle_interval and args.oracle_interval<300: parser.error('Oracle interval must be at least 300 frames')
if args.overlay and (not args.dll or args.auto_target is None):
    parser.error('Overlay test requires the automatic DLL session')
if args.oracle_poses:
    if args.measurement_seconds:parser.error("Pose oracle uses a fixed absolute frame sequence")
    args.frames=1680
process_budget=(120 if args.initialization_seconds>=100 else args.initialization_seconds)+args.measurement_seconds+30 if args.measurement_seconds else (210 if args.oracle_poses else 60)
sdk=args.sdk.resolve();output=args.output.resolve()
output.mkdir(parents=True,exist_ok=False)
exe=sdk/"bin/FFX_BRIXELIZER_GI_DX12.exe"
env=os.environ.copy()
for key in list(env):
    if key.startswith('ARC_WORKER_'): env.pop(key)
if args.worker_placement!='normal' and not args.dll: raise ValueError('Worker placement requires ARC DLL')
env['ARC_WORKER_PLACEMENT']=args.worker_placement
env['ARC_OPTIMIZER_CPU_STATE_CACHE']='1' if args.cpu_state_cache else '0'
if args.cpu_state_cache and not args.dll: raise ValueError('CPU state cache requires the ARC DLL')
if args.worker_placement=='partition': env['ARC_WORKER_ALLOW_PARTITION']='1'
env["ARC_BENCH_OUTPUT"]=str(output)
env["ARC_BENCH_ROUTE"]=args.route
env["ARC_BENCH_FRAMES"]=str(args.frames)
env["ARC_BENCH_ORACLE_INTERVAL"]=str(args.oracle_interval)
env["ARC_BENCH_ORACLE_POSES"]="1" if args.oracle_poses else "0"
for key in ('ARC_BENCH_MEASUREMENT_SECONDS','ARC_BENCH_INITIALIZATION_SECONDS'):
    env.pop(key,None)
if args.measurement_seconds:
    env['ARC_BENCH_MEASUREMENT_SECONDS']=str(args.measurement_seconds)
    env['ARC_BENCH_INITIALIZATION_SECONDS']=str(args.initialization_seconds)
env.pop("ARC_BENCH_DLL",None)
env["ARC_OPTIMIZER_LAZY_COMPILE"]="1" if args.mode=="compute-off" else "0"
env["ARC_BENCH_MODE"]=args.mode
if args.edge_threshold is not None:
    if not args.mode.startswith("compute-adaptive-") or not 0 <= args.edge_threshold <= 2:
        raise ValueError("Edge threshold 0..2 requires an adaptive compute mode")
    env["ARC_OPTIMIZER_LAZY_COMPILE"]="1" if args.mode=="compute-off" else "0"
env["ARC_BENCH_MODE"]=args.mode.replace("-hot", f"@{args.edge_threshold}-hot") if args.edge_threshold is not None else args.mode
if args.dll: env["ARC_BENCH_DLL"]=str(args.dll.resolve())
for key in ("ARC_OPTIMIZER_WORKER","ARC_OPTIMIZER_COMPILER","ARC_OPTIMIZER_CACHE"):
    env.pop(key,None)
for key in ("ARC_OPTIMIZER_CPU_TIMING","ARC_OPTIMIZER_GPU_CONTROL_TIMING"):
    env.pop(key,None)
    if args.measure_costs and args.dll: env[key]="1"
if args.dll and args.mode.startswith("compute-"):
    worker=args.shader_worker.resolve() if args.shader_worker else Path(__file__).resolve().parents[1]/"build/Release/arc-shader-tool.exe"
    compiler=args.compiler.resolve() if args.compiler else sdk/"framework/cauldron/framework/libs/dxc/bin/x64/dxcompiler.dll"
    if not worker.is_file() or not compiler.is_file():
        raise RuntimeError("Compute experiment requires built ARC shader worker and pinned DXC")
    env["ARC_OPTIMIZER_WORKER"]=str(worker)
    env["ARC_OPTIMIZER_COMPILER"]=str(compiler)
    env["ARC_OPTIMIZER_CACHE"]=str(args.cache.resolve() if args.cache else output/"shader-cache")
env.pop("ARC_AUTO_CONFIG",None)
if args.auto_target is not None:
    if not args.dll or not args.mode.startswith("compute-") or not 0 < args.auto_target <= 1000:
        raise ValueError("Automatic target requires a compute-enabled DLL and FPS in (0,1000]")
    import sys
    config=output/"automatic-config.json"
    config.write_text(json.dumps({"control_mode":args.control_mode,"quality_profile":args.quality_profile,"target_fps":args.auto_target,"diagnostics_overlay":args.overlay,"python":str(args.quality_python.resolve()) if args.quality_python else sys.executable,
        "critic":str(args.critic.resolve() if args.critic else Path(__file__).resolve().parent/"optimizer-live-quality.py"),
        "output":str(output/"automatic"),"maximum_seconds":int(process_budget-5)},indent=2))
    env["ARC_AUTO_CONFIG"]=str(config)
command=[str(exe),"-resolution","1920","1080","-benchmark",f"duration={args.frames+120}",f"path={output}","json","-screenshot"]
if args.borderless:command += ["-fullscreen"]
hashfile=lambda p:hashlib.file_digest(p.open("rb"),"sha256").hexdigest()
manifest={"host_sha256":hashfile(exe),"dll_sha256":hashfile(args.dll.resolve()) if args.dll else None,
          "mode":args.mode if args.dll else "baseline","command":command,"measured_frames":args.frames,"warmup_frames":120,
          "edge_threshold":args.edge_threshold,"effective_mode":env["ARC_BENCH_MODE"],
          "automatic_target_fps":args.auto_target,"control_mode":args.control_mode,"quality_profile":args.quality_profile,
          "initialization_seconds":args.initialization_seconds,"measurement_seconds":args.measurement_seconds,"process_budget_seconds":process_budget,
          "overlay":args.overlay,"visible":args.visible,"borderless":args.borderless,"gpu_sample_interval_seconds":args.gpu_sample_interval,
          "oracle_interval":args.oracle_interval,"oracle_poses":args.oracle_poses,"performance_run":not bool(args.oracle_poses or args.oracle_interval or args.functional),
          "cost_diagnostics":bool(args.measure_costs and args.dll),
          "cpu_state_cache":args.cpu_state_cache,
          "worker_placement":args.worker_placement,
          "compiler_sha256":hashfile(compiler) if args.dll and args.mode.startswith("compute-") else None,
          "worker_sha256":hashfile(worker) if args.dll and args.mode.startswith("compute-") else None,
          "route":args.route,"requested_gpu_power_limit_w":None,"animation_clock":"periodic_native_clip_segment_600_frames","taa_jitter":"native_callback_restored","simulation_dt":1/60,"camera_period_frames":600,"vsync":False,
          "fps_limiter":False,"upscaling":False,"frame_generation":False}
if shutil.which('nvidia-smi'):
    hardware=subprocess.run(['nvidia-smi','--query-gpu=name,uuid,pci.device_id,driver_version,enforced.power.limit','--format=csv'],capture_output=True,text=True,timeout=5,creationflags=subprocess.CREATE_NO_WINDOW)
    manifest['gpu_driver_context_csv']=hardware.stdout if hardware.returncode==0 else None
(output/"manifest.json").write_text(json.dumps(manifest,indent=2))
manifest['thermal_gate']=wait_for_cool_gpu(args.max_start_temperature)
(output/"manifest.json").write_text(json.dumps(manifest,indent=2))
startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=1 if args.visible else 0
monitor_startup=subprocess.STARTUPINFO();monitor_startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;monitor_startup.wShowWindow=4 if args.visible else 0
started=time.monotonic()
cpu=None;overlay=None;gpu_rows=[];next_gpu=0
with (output/"stdout.txt").open("w") as stdout,(output/"stderr.txt").open("w") as stderr:
    process=subprocess.Popen(command,cwd=exe.parent,env=env,stdout=stdout,stderr=stderr,startupinfo=startup)
    try:
        while process.poll() is None:
            if time.monotonic()-started>process_budget: raise subprocess.TimeoutExpired(command,process_budget)
            rows_path=output/"frames.jsonl"
            if args.overlay and overlay is None and (output/'arc.json').exists():
                overlay=subprocess.Popen([str(args.dll.resolve().parent/'arc-launcher.exe'),'--monitor',str(output/'arc.json')],startupinfo=monitor_startup,creationflags=subprocess.CREATE_NO_WINDOW)
            if args.gpu_sample_interval>0 and (args.measurement_seconds or args.frames>=600) and shutil.which("nvidia-smi") and time.monotonic()>=next_gpu:
                sample=subprocess.run(["nvidia-smi","--query-gpu=timestamp,utilization.gpu,utilization.memory,clocks.current.graphics,memory.used,temperature.gpu,power.draw,clocks.current.memory,clocks_event_reasons.active,enforced.power.limit","--format=csv,nounits"],capture_output=True,text=True,timeout=3,creationflags=subprocess.CREATE_NO_WINDOW)
                gpu_rows.append({"elapsed_s":time.monotonic()-started,"measured_phase":rows_path.exists() and rows_path.stat().st_size>0,"csv":sample.stdout,"exit_code":sample.returncode})
                with (output/'gpu-samples.jsonl').open('a') as telemetry:
                    telemetry.write(json.dumps(gpu_rows[-1])+'\n')
                next_gpu=time.monotonic()+args.gpu_sample_interval
            if cpu is None and args.process_sampler and args.frames>=600 and rows_path.exists() and rows_path.stat().st_size:
                cpu=subprocess.Popen([str(args.process_sampler.resolve()),str(process.pid),"5",str(output/"cpu.json")],stdout=subprocess.DEVNULL,stderr=stderr,creationflags=subprocess.CREATE_NO_WINDOW)
            time.sleep(0.05)
        code=process.returncode
    except subprocess.TimeoutExpired:
        process.kill();process.wait();raise RuntimeError(f"Owned benchmark exceeded {process_budget} seconds")
    finally:
        if overlay is not None:
            overlay.terminate();overlay.wait(timeout=5)
        (output/"gpu-samples.json").write_text(json.dumps(gpu_rows,indent=2))
        manifest.update(exit_code=process.poll(),process_seconds=time.monotonic()-started)
        (output/"manifest.json").write_text(json.dumps(manifest,indent=2))
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
phase_path=output/'measurement-phase.json'
manifest['measurement_phase']=json.loads(phase_path.read_text()) if phase_path.exists() else None
if manifest['measurement_phase'] is not None and not manifest['measurement_phase']['comparable']:manifest['performance_run']=False
manifest['actual_frames']=len(rows);manifest['measurement_complete']=(bool(rows) and rows[-1].get('measurement_elapsed_ms',0)>=args.measurement_seconds*1000) if args.measurement_seconds else len(rows)==args.frames
(output/"manifest.json").write_text(json.dumps(manifest,indent=2))
if not manifest['measurement_complete']: raise RuntimeError(f"Incomplete measurement: {len(rows)} frames")
fps=1000*len(rows)/sum(row["frame_ms"] for row in rows)
print(json.dumps({"output":str(output),"frames":len(rows),"fps":fps,"process_seconds":manifest["process_seconds"]}))
