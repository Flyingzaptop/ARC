"""Bounded owned-fixture runs. Commercial games are never launched."""
import os,sys,json,time,subprocess,shutil,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
W=Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine')
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=False)
exe=W/'BUILD/x64/Release/Tests/Tests.exe'
original=ROOT/'build/record-pack-control/Tests.original.exe'
shutil.copy2(exe,out/'Tests.indirect.exe')
rows=[]
try:
    configs=[('oracle',2,10)] if '--oracle-only' in sys.argv else [('oracle',2,10),('original-a',0,20),('indirect-a',1,20),('indirect-b',1,20),('original-b',0,20)]
    for name,mode,seconds in configs:
        dest=out/name;dest.mkdir()
        shutil.copy2(out/'Tests.indirect.exe',exe) # Same binary/telemetry; mode 0 retains original CPU loop.
        env={k:v for k,v in os.environ.items() if not k.startswith('ARC_')}
        env.update(ARC_WICKED_CPU_PROFILE=str(dest/'frames.csv'),ARC_WICKED_CPU_SCENE='18',ARC_WICKED_CPU_SECONDS=str(seconds),ARC_FULL_WARMUP='4',ARC_WICKED_HOOK_TIMING='0',ARC_RESIDENT_QUEUE='0',ARC_WICKED_EXPERIMENT_MODE='off',ARC_INDIRECT_MODE=str(mode))
        power_file=(dest/'gpu.csv').open('wb');power=None
        if shutil.which('nvidia-smi'):
            power=subprocess.Popen(['nvidia-smi','--query-gpu=timestamp,temperature.gpu,clocks.gr,clocks.mem,power.draw,power.limit','--format=csv','-lms','500'],stdout=power_file,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
        t=time.time()
        try:
            result=subprocess.run([str(exe),'alwaysactive','dx12'],cwd=W/'Samples/Tests',env=env,capture_output=True,timeout=60)
            (dest/'stdout.txt').write_bytes(result.stdout);(dest/'stderr.txt').write_bytes(result.stderr)
            rows.append(dict(name=name,code=result.returncode,seconds=time.time()-t,env={k:v for k,v in env.items() if k.startswith('ARC_')}))
            (out/'runs.json').write_text(json.dumps(rows,indent=2))
            if result.returncode:raise RuntimeError(f'{name} failed: {result.returncode}')
            print(name,'complete',flush=True)
        except subprocess.TimeoutExpired as e:
            (dest/'timeout.json').write_text(json.dumps(dict(timeout=60)))
            raise
        finally:
            if power:power.terminate();power.wait(timeout=5)
            power_file.close()
finally:
    shutil.copy2(original,exe)
    (out/'restoration.json').write_text(json.dumps({'original_exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()},indent=2))
