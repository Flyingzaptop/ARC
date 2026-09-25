import os,sys,subprocess,json,shutil,time
from pathlib import Path
root=Path(__file__).resolve().parents[2];w=Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine');out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=False);exe=w/'BUILD/x64/Release/Tests/Tests.exe';shutil.copy2(exe,out/'Tests.observer.exe')
env={k:v for k,v in os.environ.items() if not k.startswith('ARC_')};env.update(ARC_GPU_CANDIDATES=str(out),ARC_WICKED_CPU_PROFILE=str(out/'frames.csv'),ARC_WICKED_CPU_SCENE='18',ARC_WICKED_CPU_SECONDS='16',ARC_FULL_WARMUP='4',ARC_WICKED_HOOK_TIMING='0',ARC_RESIDENT_QUEUE='0',ARC_WICKED_EXPERIMENT_MODE='off')
try:
    t=time.time();p=subprocess.run([str(exe),'alwaysactive','dx12'],cwd=w/'Samples/Tests',env=env,capture_output=True,timeout=60);(out/'stdout.txt').write_bytes(p.stdout);(out/'stderr.txt').write_bytes(p.stderr);(out/'run.json').write_text(json.dumps({'code':p.returncode,'seconds':time.time()-t,'environment':{k:v for k,v in env.items() if k.startswith('ARC_')}},indent=2));print('native',p.returncode,flush=True)
    if p.returncode:raise RuntimeError('native observer failed')
    subprocess.run([sys.executable,str(root/'scripts/gpu_candidate_pipeline.py'),str(out),str(out/'result.json')],check=True)
finally:shutil.copy2(root/'build/record-pack-control/Tests.original.exe',exe)
