"""Full session A/B/B/A. Source-assisted Wicked adapter, generic Cauldron GPU only."""
import argparse,os,json,time,subprocess,hashlib,sys,shutil
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument("output",type=Path);p.add_argument("--renderer",choices=["wicked","cauldron"],required=True);p.add_argument("--runs",default="A1,B1,B2,A2");p.add_argument("--warmup",type=int,default=20);p.add_argument("--seconds",type=int,default=20);p.add_argument("--oracle",action="store_true");p.add_argument("--static-camera",action="store_true");p.add_argument("--no-offload",action="store_true");p.add_argument("--package",type=Path);p.add_argument("--stop-after",type=float);p.add_argument("--target",type=float,default=120)
a=p.parse_args()
if not 1<=a.seconds<=60 or not 0<=a.warmup<=120 or not 0<a.target<=1000:p.error("Invalid bounded durations or target")
if a.renderer=="wicked" and a.warmup+a.seconds>120:p.error("Wicked process measurement budget exceeds 120 seconds")
r=Path(__file__).resolve().parents[1];root=Path("C:/Users/r3d_flzp/ARC-Hardening-GPU");w=root/"WickedEngine";sdk=root/"benchmarks/FidelityFX-1.1.4/sdk"
dll=r/"build/Release/arc-dx12-probe.dll";compiler=root/"universal-optimizer/deps/dxc-1.9.2602.24/bin/x64/dxcompiler.dll"
worker=r/"build/Release/arc-shader-tool.exe"
if a.package:
 dll=a.package/"arc-dx12-probe.dll";worker=a.package/"arc-shader-tool.exe"
a.output.mkdir(parents=True,exist_ok=True)
def digest(f):return hashlib.file_digest(f.open("rb"),"sha256").hexdigest()
for name in a.runs.split(","):
 out=a.output/name;out.mkdir();full=name.startswith("B");env={k:v for k,v in os.environ.items() if not k.startswith("ARC_")}
 if a.renderer=="cauldron":
  cmd=[sys.executable,str(r/"scripts/run-cauldron-benchmark.py"),str(sdk),str(out/"session"),"--mode","compute-neutral-hot","--initialization-seconds",str(max(100,a.warmup)),"--measurement-seconds",str(a.seconds),"--visible"]
  if full:cmd += ["--dll",str(dll),"--compiler",str(compiler),"--shader-worker",str(worker),"--auto-target",str(a.target),"--control-mode","target_feedback","--quality-profile","aggressive"]
  subprocess.run(cmd,env=env,check=True);continue
 exe=w/"BUILD/x64/Release/Tests/Tests.exe"
 config={"control_mode":"target_feedback","quality_profile":"aggressive","target_fps":a.target,"host_cpu_offload":not a.no_offload,"output":str(out/"automatic"),"maximum_seconds":a.warmup+a.seconds+25,"python":sys.executable,"critic":str(r/"scripts/optimizer-live-quality.py")}
 (out/"automatic-config.json").write_text(json.dumps(config,indent=2))
 env.update(ARC_WICKED_EXPERIMENT_MODE="off",ARC_WICKED_CPU_PROFILE=str(out/"cpu.csv"),ARC_WICKED_CPU_SCENE="18",ARC_WICKED_CPU_SECONDS=str(a.warmup+a.seconds),ARC_WICKED_HOOK_TIMING="0",ARC_FULL_WARMUP=str(a.warmup),ARC_BENCH_OUTPUT=str(out),ARC_RESIDENT_QUEUE=("2" if a.oracle else "1") if full and not a.no_offload else "0",ARC_WICKED_DYNAMIC_CAMERA="0" if a.static_camera else "1")
 if a.stop_after is not None:env["ARC_FULL_STOP_AFTER_MS"]=str(int(a.stop_after*1000))
 if full:env.update(ARC_BENCH_DLL=str(dll),ARC_AUTO_CONFIG=str(out/"automatic-config.json"),ARC_OPTIMIZER_WORKER=str(worker),ARC_OPTIMIZER_COMPILER=str(compiler),ARC_OPTIMIZER_CACHE=str(out/"shader-cache"),ARC_OPTIMIZER_LAZY_COMPILE="0")
 manifest={"renderer":a.renderer,"run":name,"full_arc":full,"host_cpu_offload":full and not a.no_offload,"legacy_host_quality":"off","generic_gpu_optimizer":full,"target_fps":a.target,"warmup_seconds":a.warmup,"measurement_seconds":a.seconds,"oracle":a.oracle,"exe_sha256":digest(exe),"dll_sha256":digest(dll) if full else None,"environment":env,"arguments":["alwaysactive","dx12"],"vsync":False,"fps_definition":"successful Present return frequency; not displayed FPS","power_limit_set":False}
 # Keep environment manifest task-specific, never publish unrelated inherited values.
 manifest["environment"]={k:v for k,v in env.items() if k.startswith("ARC_")}
 (out/"manifest.json").write_text(json.dumps(manifest,indent=2))
 with (out/"hardware.csv").open("w") as hw,(out/"stdout.txt").open("w") as stdout,(out/"stderr.txt").open("w") as stderr:
  sampler=subprocess.Popen(["nvidia-smi","--query-gpu=timestamp,temperature.gpu,power.draw,clocks.gr,clocks.mem,utilization.gpu","--format=csv","--loop=1"],stdout=hw,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
  try:
   process=subprocess.Popen([str(exe),"alwaysactive","dx12"],cwd=w/"Samples/Tests",env=env,stdout=stdout,stderr=stderr)
   try:code=process.wait(timeout=a.warmup+a.seconds+35)
   except subprocess.TimeoutExpired:process.kill();process.wait();raise
  finally:sampler.terminate();sampler.wait()
 manifest["exit_code"]=code;(out/"manifest.json").write_text(json.dumps(manifest,indent=2))
 if code:raise RuntimeError(f"{name}: exit {code}")
 print(f"Completed {a.renderer} {name}",flush=True)
