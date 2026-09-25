"""Bounded native captures selected automatically from the prior runtime detector."""
import argparse,subprocess,os,sys,json,time,shutil,hashlib,csv,io
from pathlib import Path
from cpu_parallel_screen import record_attempt
p=argparse.ArgumentParser();p.add_argument('probe',type=Path);p.add_argument('output',type=Path);p.add_argument('--limit',type=int);p.add_argument('--context',type=Path);p.add_argument('--measurements',type=Path);p.add_argument('--mode',choices=['trace','boundary','training','consumer'],default='trace');p.add_argument('--events',type=int,default=32768);p.add_argument('--ms',type=int,default=100);p.add_argument('--max-arg-span',type=int,default=0);p.add_argument('--start',type=int,default=0);p.add_argument('--outermost',action='store_true');p.add_argument('--stop-unsupported',action='store_true');a=p.parse_args();r=Path(__file__).resolve().parents[1]
subprocess.run([sys.executable,str(r/'scripts/select-cpu-contracts.py'),str(a.probe),str(a.output),"--mode",a.mode,"--events",str(a.events),"--ms",str(a.ms),"--max-arg-span",str(a.max_arg_span)]+(["--outermost"] if a.outermost else [])+(["--stop-unsupported"] if a.stop_unsupported else [])+(["--context",str(a.context)] if a.context else [])+(["--measurements",str(a.measurements)] if a.measurements else []),check=True)
plan=json.loads((a.output/'selection.json').read_text());w=Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine');exe=w/'BUILD/x64/Release/Tests/Tests.exe';backup=a.output/'Tests-before.exe';source=Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/full-arc-20260924/package/Tests.exe')
if hashlib.sha256(source.read_bytes()).hexdigest()!=plan['image_sha256']:raise ValueError('Fixture generation differs from detector')
shutil.copy2(exe,backup)
shutil.copy2(r/'build/Release/arc-cpu-contract-capture.dll',a.output/'capture.dll')
shutil.copy2(r/'src/backends/cpu/contract_capture.cpp',a.output/'capture-source.cpp')
try:
 shutil.copy2(source,exe)
 for selected in plan['candidates'][a.start:a.start+a.limit if a.limit is not None else None]:
  out=a.output/selected['directory'];env={k:v for k,v in os.environ.items() if not k.startswith('ARC_')}
  env.update(ARC_WICKED_EXPERIMENT_MODE='off',ARC_WICKED_CPU_PROFILE=str(out/'cpu.csv'),ARC_WICKED_CPU_SCENE='18',ARC_WICKED_CPU_SECONDS='65' if a.mode in ['training','consumer'] else '20',ARC_FULL_WARMUP='4',ARC_WICKED_HOOK_TIMING='0',ARC_RESIDENT_QUEUE='0')
  started=time.monotonic();process=subprocess.Popen([str(exe),'alwaysactive','dx12'],cwd=w/'Samples/Tests',env=env)
  try:
   # Fixture readiness only, never an input to candidate classification. A fixed
   # 4-second sleep armed the two-second observer during scene construction.
   ready_frames=0
   while time.monotonic()-started<16 and process.poll() is None:
    try:
     if (out/'cpu.csv').stat().st_size>8*1024*1024:raise RuntimeError('Readiness telemetry exceeded 8 MiB budget')
     text=(out/'cpu.csv').read_text();switched=False;ready_frames=0
     for row in csv.DictReader(io.StringIO(text)):
      if row.get('event')=='Scene switch':switched=True;ready_frames=0
      elif switched and row.get('event')=='Application Render':ready_frames+=1
     if ready_frames>=5:break
    except (OSError,ValueError):pass
    time.sleep(.1)
   if ready_frames<5:raise RuntimeError('Owned fixture did not reach five post-load render frames within 16 seconds')
   with (out/'attach.log').open('w') as log:
    attach=subprocess.run([str(r/'build/Release/arc-dx12-probe-launch.exe'),'--attach',str(process.pid),str(r/'build/Release/arc-cpu-contract-capture.dll'),str(out/'session.json')],stdout=log,stderr=log,timeout=8)
   until=started+150;ready=None
   while process.poll() is None and time.monotonic()<until:
    if (out/'capture.json').exists():
     try:
      result=json.loads((out/'capture.json').read_text())
      if not result.get('trap_cleanup_pending') and ready is None:ready=time.monotonic()
     except (OSError,ValueError):pass
    if ready is not None and time.monotonic()-ready>2:
     import ctypes
     callback=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p)
     @callback
     def close_window(hwnd,param):
      pid=ctypes.c_ulong();ctypes.windll.user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd),ctypes.byref(pid))
      if pid.value==process.pid:ctypes.windll.user32.PostMessageW(ctypes.c_void_p(hwnd),0x10,0,0)
      return True
     ctypes.windll.user32.EnumWindows(close_window,0)
     break
    time.sleep(.05)
   code=process.wait(timeout=10);manifest={'selected':selected,'pid':process.pid,'exe_sha256':plan['image_sha256'],'capture_dll_sha256':hashlib.sha256((r/'build/Release/arc-cpu-contract-capture.dll').read_bytes()).hexdigest(),'attach_code':attach.returncode,'exit_code':code,'seconds':time.monotonic()-started,'observation_only':True,'original_work_executed':True}
   (out/'run.json').write_text(json.dumps(manifest,indent=2));record_attempt(a.measurements or a.probe/'parallel-measurements.json',plan['image_sha256'],selected['ids'],json.loads((a.output/'parallel-screen.json').read_text())['candidates'],out/'capture.json');print(selected['directory'],attach.returncode,code,flush=True)
   if code:raise RuntimeError('Owned renderer failed; stop captures')
  finally:
   if process.poll() is None:process.terminate();process.wait(timeout=5)
finally:shutil.copy2(backup,exe)
