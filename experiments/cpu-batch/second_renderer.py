"""Owned second-renderer probe and foreign-contract rejection smoke test."""
import argparse,subprocess,time,json,shutil,os,sys,hashlib
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('foreign_contract',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);root=Path(__file__).resolve().parents[2]
for mode in ['original','observe_reject','original2']:
 out=a.output/mode
 env={k:v for k,v in os.environ.items() if not k.startswith('ARC_')}
 host=subprocess.Popen([str(root/'build/Release/arc-dynamic-city.exe'),str(out),'120','--seconds','12'],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 began=time.monotonic();manifest={'mode':mode,'replacement_enabled':False}
 try:
  if mode=='observe_reject':
   time.sleep(2)
   probe=subprocess.run([str(root/'build/Release/arc-cpu-task-probe.exe'),str(host.pid),'5',str(a.output/'probe')],capture_output=True,timeout=12)
   manifest['probe_exit']=probe.returncode
   reject=a.output/'foreign-contract';reject.mkdir()
   for name in ['request.txt','expected-code.bin']:shutil.copy2(a.foreign_contract/name,reject/name)
   attach=subprocess.run([str(root/'build/Release/arc-dx12-probe-launch.exe'),'--attach',str(host.pid),str(root/'build/Release/arc-cpu-contract-capture.dll'),str(reject/'session.json')],capture_output=True,timeout=8)
   (reject/'attach.txt').write_bytes(attach.stdout+attach.stderr);manifest['attach_exit']=attach.returncode
   manifest['rejection_verified']=attach.returncode!=0 and not (reject/'capture.json').exists()
  text,_=host.communicate(timeout=max(1,45-(time.monotonic()-began)));(a.output/f'{mode}.log').write_bytes(text);manifest['exit_code']=host.returncode
  if host.returncode:raise RuntimeError('second renderer failure')
 finally:
  if host.poll() is None:host.terminate();host.wait(timeout=5)
  manifest['seconds']=time.monotonic()-began;(a.output/f'{mode}.json').write_text(json.dumps(manifest,indent=2))
 if mode=='observe_reject':
  subprocess.run([sys.executable,str(root/'scripts/discover-cpu-tasks.py'),str(a.output/'probe')],check=True)
  subprocess.run([sys.executable,str(root/'scripts/cpu_parallel_screen.py'),str(a.output/'probe'),str(a.output/'ranking.json')],check=True)
 print(mode,manifest,flush=True)
