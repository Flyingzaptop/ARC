"""Owned renderer compatibility experiment, each process bounded to 60 seconds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import sys
import shutil
from benchmark_thermal_gate import wait_for_cool_gpu

parser=argparse.ArgumentParser()
parser.add_argument("wicked",type=Path)
parser.add_argument("output",type=Path)
parser.add_argument("--dll",type=Path)
parser.add_argument("--compiler",type=Path)
parser.add_argument("--compute-mode",choices=['off','neutral','neutral|heaviest','1x2|heaviest','2x2|heaviest','adaptive-2x2@0.9|heaviest'],default='neutral|heaviest')
parser.add_argument("--auto-target",type=float)
parser.add_argument("--measure-costs",action="store_true")
parser.add_argument("--seconds",type=int,choices=range(10,61),default=10)
parser.add_argument("--initialization-seconds",type=int,choices=range(2,121),default=20)
parser.add_argument("--quality-profile",choices=["balanced","aggressive"],default="balanced")
parser.add_argument("--visible",action="store_true")
parser.add_argument("--worker-placement",choices=['normal','prefer','core','partition','adaptive'],default='normal')
parser.add_argument("--max-start-temperature",type=float)
parser.add_argument("--focus-scene",choices=['hello','instances'])
parser.add_argument("--dynamic-camera",action="store_true")
args=parser.parse_args()
process_budget=args.initialization_seconds+args.seconds+30
if args.initialization_seconds>20 and not args.focus_scene:parser.error("Long initialization requires a single focus scene")
root=args.wicked.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
exe=root/"BUILD/x64/Release/Tests/Tests.exe"
env=os.environ.copy()
for key in list(env):
    if key.startswith(('ARC_WICKED_','ARC_BENCH_','ARC_OPTIMIZER_','ARC_AUTO_','ARC_WORKER_')): env.pop(key)
if args.worker_placement!='normal' and not args.dll: raise ValueError('Worker placement requires ARC DLL')
env['ARC_WORKER_PLACEMENT']=args.worker_placement
env['ARC_OPTIMIZER_LAZY_COMPILE']='1' if args.compute_mode=='off' else '0'
if args.worker_placement=='partition': env['ARC_WORKER_ALLOW_PARTITION']='1'
env.update(ARC_WICKED_EXPERIMENT_MODE='off',ARC_WICKED_OUTPUT=str(output/'renderer.json'),ARC_WICKED_SECONDS=str(args.seconds),ARC_WICKED_WARMUP_SECONDS=str(args.initialization_seconds),ARC_WICKED_SCENE_SETTLE_MS='500',ARC_WICKED_HOOK_TIMING='0')
env['ARC_BENCH_OUTPUT']=str(output)
if args.focus_scene: env['ARC_WICKED_FOCUS_SCENE']='0' if args.focus_scene=='hello' else '18'
if args.dynamic_camera: env['ARC_WICKED_DYNAMIC_CAMERA']='1'
if args.dll:
    if not args.compiler or not args.compiler.is_file(): raise ValueError("Pinned DXC required")
    env.update(ARC_BENCH_DLL=str(args.dll.resolve()),ARC_BENCH_OUTPUT=str(output),ARC_BENCH_COMPUTE=args.compute_mode,
               ARC_OPTIMIZER_WORKER=str(Path(__file__).resolve().parents[1]/'build/Release/arc-shader-tool.exe'),
               ARC_OPTIMIZER_COMPILER=str(args.compiler.resolve()),ARC_OPTIMIZER_CACHE=str(output/'shader-cache'))
    if args.measure_costs:
        env.update(ARC_OPTIMIZER_CPU_TIMING='1',ARC_OPTIMIZER_GPU_CONTROL_TIMING='1')
if args.auto_target is not None:
    if not args.dll or not 0 < args.auto_target <= 1000:
        raise ValueError('Automatic target requires a DLL and FPS in (0,1000]')
    config=output/'automatic-config.json'
    config.write_text(json.dumps({'quality_profile':args.quality_profile,'target_fps':args.auto_target,'python':sys.executable,
        'critic':str(Path(__file__).resolve().parent/'optimizer-live-quality.py'),
        'output':str(output/'automatic'),'maximum_seconds':process_budget-5},indent=2))
    env['ARC_AUTO_CONFIG']=str(config)
manifest={'host_quality_actions':'off','native_resolution':[1920,1080],'dll_sha256':hashlib.sha256(args.dll.read_bytes()).hexdigest() if args.dll else None,
          'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'mode':args.compute_mode if args.dll else 'baseline',
          'automatic_target_fps':args.auto_target,'cost_diagnostics':bool(args.measure_costs and args.dll),
          'measurement_seconds':args.seconds,'initialization_seconds':args.initialization_seconds,'process_budget_seconds':process_budget,'quality_profile':args.quality_profile,
          'worker_placement':args.worker_placement,
          'focus_scene':args.focus_scene,'dynamic_camera':args.dynamic_camera,
          'compiler_sha256':hashlib.sha256(args.compiler.read_bytes()).hexdigest() if args.dll else None}
startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=1 if args.visible else 0
manifest['thermal_gate']=wait_for_cool_gpu(args.max_start_temperature)
def hardware_snapshot():
    if not shutil.which('nvidia-smi'):return None
    return subprocess.run(['nvidia-smi','--query-gpu=name,driver_version,enforced.power.limit,clocks.current.graphics,clocks.current.memory,temperature.gpu,power.draw','--format=csv'],capture_output=True,text=True,timeout=5,creationflags=subprocess.CREATE_NO_WINDOW).stdout
manifest['hardware_before_csv']=hardware_snapshot()
start=time.monotonic()
history=[]
with (output/'stdout.txt').open('w') as out,(output/'stderr.txt').open('w') as err:
    process=subprocess.Popen([str(exe),'alwaysactive','dx12'],cwd=root/'Samples/Tests',env=env,stdout=out,stderr=err,startupinfo=startup)
    try:
        while process.poll() is None:
            remaining=process_budget-(time.monotonic()-start)
            if remaining<=0: raise subprocess.TimeoutExpired(str(exe),process_budget)
            try: process.wait(timeout=min(.5,remaining))
            except subprocess.TimeoutExpired: pass
            if args.dll and args.focus_scene and (output/'arc.json').is_file():
                try:
                    snapshot=json.loads((output/'arc.json').read_text())
                    history.append({'elapsed_seconds':time.monotonic()-start,'presents':snapshot.get('present_calls'),
                                    'worker_placement':snapshot.get('worker_placement'),
                                    'automatic_session':snapshot.get('automatic_session')})
                except (OSError,ValueError): pass # an atomic writer may be replacing its snapshot
        code=process.returncode
    except subprocess.TimeoutExpired:
        process.kill();process.wait();raise RuntimeError(f'Owned renderer exceeded {process_budget} seconds')
(output/'adaptation-history.json').write_text(json.dumps(history,indent=2))
manifest.update(exit_code=code,seconds=time.monotonic()-start,hardware_after_csv=hardware_snapshot())
(output/'manifest.json').write_text(json.dumps(manifest,indent=2))
if code: raise RuntimeError(f'Owned renderer failed: {code}')
print(json.dumps({'output':str(output),'mode':manifest['mode'],'worker_placement':args.worker_placement,
                  'seconds':manifest['seconds'],'exit_code':code,
                  'cooldown_seconds':manifest['thermal_gate'].get('wait_seconds',0)}))
