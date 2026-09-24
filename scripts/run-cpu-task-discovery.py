"""Owned Wicked observation gate: baseline, then bounded automatic discovery."""
import argparse,os,subprocess,time,shutil,json,hashlib,sys,csv,statistics
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--runs',default='original,discovery');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
r=Path(__file__).resolve().parents[1];w=Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine');exe=w/'BUILD/x64/Release/Tests/Tests.exe';source=Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/full-arc-20260924/package/Tests.exe');backup=a.output/'Tests-before.exe';shutil.copy2(exe,backup)
try:
 shutil.copy2(source,exe)
 for name in a.runs.split(','):
  out=a.output/name;out.mkdir();env={k:v for k,v in os.environ.items() if not k.startswith('ARC_')}
  env.update(ARC_WICKED_EXPERIMENT_MODE='off',ARC_WICKED_CPU_PROFILE=str(out/'cpu.csv'),ARC_WICKED_CPU_SCENE='18',ARC_WICKED_CPU_SECONDS='20',ARC_FULL_WARMUP='8',ARC_WICKED_HOOK_TIMING='0',ARC_RESIDENT_QUEUE='0')
  manifest={'mode':name,'exe_sha256':hashlib.file_digest(exe.open('rb'),'sha256').hexdigest(),'sampler_sha256':hashlib.file_digest((r/'build/Release/arc-cpu-task-probe.exe').open('rb'),'sha256').hexdigest(),'warmup_seconds':8,'measurement_seconds':12,'replacement_enabled':False,'names_or_addresses_supplied_to_detector':False,'environment':{k:v for k,v in env.items() if k.startswith('ARC_')}}
  started=time.monotonic();process=subprocess.Popen([str(exe),'alwaysactive','dx12'],cwd=w/'Samples/Tests',env=env);helper=None
  try:
   if name=='discovery':
    while time.monotonic()-started<8 and process.poll() is None:time.sleep(.05)
    manifest['probe_start_process_seconds']=time.monotonic()-started
    helper=subprocess.Popen([str(r/'build/Release/arc-cpu-task-probe.exe'),str(process.pid),'10',str(out/'probe')]);helper.wait(timeout=18)
    if helper.returncode:raise RuntimeError('Probe failed '+str(helper.returncode))
    subprocess.run([sys.executable,str(r/'scripts/discover-cpu-tasks.py'),str(out/'probe')],check=True)
   code=process.wait(timeout=max(1,35-(time.monotonic()-started)));manifest['exit_code']=code
   if code:raise RuntimeError('Host failed '+str(code))
  finally:
   # The native probe's RAII resumes its own suspensions before exit. Let it exit before killing host.
   if helper and helper.poll() is None:helper.wait(timeout=18)
   if process.poll() is None:process.terminate();process.wait(timeout=5)
   manifest['process_seconds']=time.monotonic()-started;(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
  with (out/'cpu.csv').open() as f:rows=list(csv.DictReader(f))
  measured={int(x['frame']) for x in rows if x['event']=='CPU processing through submission ms' and 8000<=float(x['elapsed_ms'])<20000}
  metrics={}
  for event in ['CPU processing through submission ms','Retired GPU frame span ms','Controlled Present interval']:
   v=sorted(float(x['ms']) for x in rows if x['event']==event and int(x['frame']) in measured)
   metrics[event]={'count':len(v),'mean':statistics.mean(v),'median':statistics.median(v),'p95':v[int((len(v)-1)*.95)],'p99':v[int((len(v)-1)*.99)],'max':max(v)} if v else None
  if metrics['Controlled Present interval']:metrics['present_hz']=1000/metrics['Controlled Present interval']['mean']
  (out/'timings.json').write_text(json.dumps(metrics,indent=2))
  print('Completed',name,flush=True)
finally:shutil.copy2(backup,exe)
