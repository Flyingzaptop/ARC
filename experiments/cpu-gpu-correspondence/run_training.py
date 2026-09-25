import os,sys,json,subprocess,shutil,time
from pathlib import Path
root=Path(__file__).resolve().parents[2];w=Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine');out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=False)
exe=w/'BUILD/x64/Release/Tests/Tests.exe';shutil.copy2(exe,out/'Tests.training.exe');runs=[]
try:
    for name,mode in [('busy',2),('capture',0)]:
        d=out/name;d.mkdir();env={k:v for k,v in os.environ.items() if not k.startswith('ARC_')};env.update(ARC_WICKED_CPU_PROFILE=str(d/'frames.csv'),ARC_WICKED_CPU_SCENE='18',ARC_WICKED_CPU_SECONDS='12',ARC_FULL_WARMUP='4',ARC_WICKED_HOOK_TIMING='0',ARC_RESIDENT_QUEUE='0',ARC_WICKED_EXPERIMENT_MODE='off',ARC_INDIRECT_MODE=str(mode))
        if name=='busy':env['ARC_INDIRECT_BUSY_TEST']='1'
        else:env['ARC_PACK_GPU_AUDIT']=str(d/'audit')
        t=time.time();r=subprocess.run([str(exe),'alwaysactive','dx12'],cwd=w/'Samples/Tests',env=env,capture_output=True,timeout=60);(d/'stdout.txt').write_bytes(r.stdout);(d/'stderr.txt').write_bytes(r.stderr);runs.append(dict(name=name,code=r.returncode,seconds=time.time()-t,environment={k:v for k,v in env.items() if k.startswith('ARC_')}));(out/'runs.json').write_text(json.dumps(runs,indent=2));print(name,r.returncode,flush=True)
        if r.returncode:raise RuntimeError('native diagnostic failed')
    subprocess.run([sys.executable,str(root/'experiments/cpu-gpu-correspondence/make_input.py'),str(out/'capture')],check=True)
    subprocess.run([sys.executable,str(root/'scripts/cpu_gpu_correspondence.py'),str(out/'capture/input.json'),str(out/'discovery.json')],check=True)
finally:
    shutil.copy2(root/'build/record-pack-control/Tests.original.exe',exe)
