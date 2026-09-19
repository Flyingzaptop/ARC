"""Run the owned Cauldron test host, with a 60 second process budget."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import shutil

parser=argparse.ArgumentParser()
parser.add_argument("sdk",type=Path)
parser.add_argument("output",type=Path)
parser.add_argument("--dll",type=Path)
parser.add_argument("--frames",type=int,default=600)
parser.add_argument("--process-sampler",type=Path)
args=parser.parse_args()
sdk=args.sdk.resolve();output=args.output.resolve()
output.mkdir(parents=True,exist_ok=False)
exe=sdk/"bin/FFX_BRIXELIZER_GI_DX12.exe"
env=os.environ.copy()
env["ARC_BENCH_OUTPUT"]=str(output)
env["ARC_BENCH_FRAMES"]=str(args.frames)
env.pop("ARC_BENCH_DLL",None)
if args.dll: env["ARC_BENCH_DLL"]=str(args.dll.resolve())
command=[str(exe),"-resolution","1920","1080","-benchmark",f"duration={args.frames+120}",f"path={output}","json","-screenshot"]
hashfile=lambda p:hashlib.file_digest(p.open("rb"),"sha256").hexdigest()
manifest={"host_sha256":hashfile(exe),"dll_sha256":hashfile(args.dll.resolve()) if args.dll else None,
          "command":command,"measured_frames":args.frames,"warmup_frames":120,
          "simulation_dt":1/60,"camera_period_frames":600,"vsync":False,
          "fps_limiter":False,"upscaling":False,"frame_generation":False}
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
if len(rows)!=args.frames: raise RuntimeError(f"Incomplete measurement: {len(rows)}/{args.frames}")
fps=1000*len(rows)/sum(row["frame_ms"] for row in rows)
print(json.dumps({"output":str(output),"frames":len(rows),"fps":fps,"process_seconds":manifest["process_seconds"]}))
