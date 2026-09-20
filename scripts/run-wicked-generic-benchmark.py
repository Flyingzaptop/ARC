"""Owned renderer compatibility experiment, each process bounded to 60 seconds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import sys

parser=argparse.ArgumentParser()
parser.add_argument("wicked",type=Path)
parser.add_argument("output",type=Path)
parser.add_argument("--dll",type=Path)
parser.add_argument("--compiler",type=Path)
parser.add_argument("--compute-mode",choices=['neutral','neutral|heaviest','1x2|heaviest','2x2|heaviest','adaptive-2x2@0.9|heaviest'],default='neutral|heaviest')
parser.add_argument("--auto-target",type=float)
parser.add_argument("--measure-costs",action="store_true")
parser.add_argument("--seconds",type=int,choices=range(1,36),default=10)
args=parser.parse_args()
root=args.wicked.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
exe=root/"BUILD/x64/Release/Tests/Tests.exe"
env=os.environ.copy()
for key in list(env):
    if key.startswith(('ARC_WICKED_','ARC_BENCH_','ARC_OPTIMIZER_','ARC_AUTO_')): env.pop(key)
env.update(ARC_WICKED_EXPERIMENT_MODE='off',ARC_WICKED_OUTPUT=str(output/'renderer.json'),ARC_WICKED_SECONDS=str(args.seconds),ARC_WICKED_WARMUP_SECONDS='2',ARC_WICKED_SCENE_SETTLE_MS='500',ARC_WICKED_HOOK_TIMING='0')
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
    config.write_text(json.dumps({'target_fps':args.auto_target,'python':sys.executable,
        'critic':str(Path(__file__).resolve().parent/'optimizer-live-quality.py'),
        'output':str(output/'automatic'),'maximum_seconds':50},indent=2))
    env['ARC_AUTO_CONFIG']=str(config)
manifest={'host_quality_actions':'off','native_resolution':[1920,1080],'dll_sha256':hashlib.sha256(args.dll.read_bytes()).hexdigest() if args.dll else None,
          'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'mode':args.compute_mode if args.dll else 'baseline',
          'automatic_target_fps':args.auto_target,'cost_diagnostics':bool(args.measure_costs and args.dll),
          'measurement_seconds':args.seconds,
          'compiler_sha256':hashlib.sha256(args.compiler.read_bytes()).hexdigest() if args.dll else None}
startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
start=time.monotonic()
with (output/'stdout.txt').open('w') as out,(output/'stderr.txt').open('w') as err:
    process=subprocess.Popen([str(exe),'alwaysactive','dx12'],cwd=root/'Samples/Tests',env=env,stdout=out,stderr=err,startupinfo=startup)
    try: code=process.wait(timeout=60)
    except subprocess.TimeoutExpired:
        process.kill();process.wait();raise RuntimeError('Owned renderer exceeded 60 seconds')
manifest.update(exit_code=code,seconds=time.monotonic()-start)
(output/'manifest.json').write_text(json.dumps(manifest,indent=2))
if code: raise RuntimeError(f'Owned renderer failed: {code}')
print(json.dumps(manifest))
