"""Live automatic binding -> bounded CPU write watch. No oracle input."""
import argparse,json,os,sys,time,subprocess,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'scripts'))
from gpu_candidate_pipeline import analyze
from cpu_producer_plan import plan
args=argparse.ArgumentParser();args.add_argument('output',type=Path);args.add_argument('host',type=Path);args.add_argument('--watch-events',type=int,default=6);args.add_argument('--watch-ms',type=int,default=3000);args.add_argument('--slice-calls',type=int,default=3);args.add_argument('--slice-ms',type=int,default=5000);args.add_argument('--event-buffer-budget-kib',type=int,default=1024);opt=args.parse_args()
if not(1<=opt.watch_events<=32 and 1<=opt.watch_ms<=10000 and 1<=opt.slice_calls<=4 and 1<=opt.slice_ms<=10000):raise ValueError('capture budget')
if opt.event_buffer_budget_kib*1024<770048:raise ValueError('requested budget below fixed x64 event-buffer reservation; no process launched')
W=Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine');out=opt.output;out.mkdir(parents=True,exist_ok=False)
exe=W/'BUILD/x64/Release/Tests/Tests.exe';source=opt.host;shutil.copy2(source,exe)
live=out/'live';live.mkdir();frozen=out/'binding';frozen.mkdir();watch=out/'watch';watch.mkdir()
env={k:v for k,v in os.environ.items() if not k.startswith('ARC_')};env.update(ARC_GPU_CANDIDATES=str(live),ARC_WICKED_CPU_PROFILE=str(live/'frames.csv'),ARC_WICKED_CPU_SCENE='18',ARC_WICKED_CPU_SECONDS='45',ARC_FULL_WARMUP='4',ARC_WICKED_EXPERIMENT_MODE='off',ARC_RESIDENT_QUEUE='0',ARC_WICKED_HOOK_TIMING='0')
p=None
try:
    p=subprocess.Popen([str(exe),'alwaysactive','dx12'],cwd=W/'Samples/Tests',env=env,stdout=(out/'stdout.txt').open('wb'),stderr=(out/'stderr.txt').open('wb'));start=time.monotonic()
    while time.monotonic()-start<25 and p.poll() is None:
        try:
            raw=(live/'events.jsonl').read_bytes();raw=raw[:raw.rfind(b'\n')+1];events=[json.loads(x) for x in raw.splitlines()]
            if sum(e['event']=='completed' for e in events)>=3:break
        except (OSError,ValueError):pass
        time.sleep(.05)
    else:raise RuntimeError('no completed candidate observations')
    for f in live.glob('s*'):
        if f.is_file():os.link(f,frozen/f.name)
    (frozen/'events.jsonl').write_bytes(raw);result=analyze(frozen);(frozen/'result.json').write_text(json.dumps(result,indent=2));binding=next(b for b in result['bindings'] if b['status']=='confirmed' and b['live_binding_valid']);relation=next(r for r in binding['discovery']['relations'] if r['subsample_of'] is None and r['confirmation']=='confirmed_on_new_write')
    target=int(relation['cpu_region'])+relation['cpu_offset'];(watch/'binding.json').write_text(json.dumps({'pid':p.pid,'binding':binding,'relation':relation,'target':target},indent=2));(watch/'request.txt').write_text(f'{target:x} {relation["width"]} {opt.watch_events} {opt.watch_ms}\n')
    dll=ROOT/'build/Release/arc-cpu-producer-watch.dll';attach=subprocess.run([str(ROOT/'build/Release/arc-dx12-probe-launch.exe'),'--attach',str(p.pid),str(dll),str(watch/'session.json')],capture_output=True,timeout=10);(watch/'attach.txt').write_bytes(attach.stdout+attach.stderr)
    if attach.returncode:raise RuntimeError('attach failed')
    until_ready=time.monotonic()+3
    while not (watch/'ready.json').exists() and time.monotonic()<until_ready:time.sleep(.002)
    (live/'producer-followup.txt').write_text(f"{binding['resource']} {binding['generation']} 4\n")
    until=time.monotonic()+opt.watch_ms/1000+5
    while not (watch/'done.json').exists() and time.monotonic()<until and p.poll() is None:
        current=(live/'events.jsonl').read_text().splitlines()
        for line in current:
            try:e=json.loads(line)
            except ValueError:continue
            if e['event']=='invalidate' and e['resource']==binding['resource'] and e.get('generation',binding['generation'])==binding['generation']:(watch/'cancel').touch()
        time.sleep(.02)
    if not (watch/'done.json').exists():raise RuntimeError('watch did not stop')
    slice_dir=out/'slice';slice_plan=plan(watch,slice_dir,opt.slice_calls,opt.slice_ms)
    (live/'producer-followup.txt').write_text(f"{binding['resource']} {binding['generation']} 6\n")
    trace_attach=subprocess.run([str(ROOT/'build/Release/arc-dx12-probe-launch.exe'),'--attach',str(p.pid),str(ROOT/'build/Release/arc-cpu-producer-slice.dll'),str(slice_dir/'session.json')],capture_output=True,timeout=10);(slice_dir/'attach.txt').write_bytes(trace_attach.stdout+trace_attach.stderr)
    if trace_attach.returncode:raise RuntimeError('slice attach failed')
    deadline=time.monotonic()+opt.slice_ms/1000+5
    while not (slice_dir/'done.json').exists() and time.monotonic()<deadline and p.poll() is None:
        for line in (live/'events.jsonl').read_text().splitlines():
            try:e=json.loads(line)
            except ValueError:continue
            if e['event']=='invalidate' and e['resource']==binding['resource'] and e.get('generation',binding['generation'])==binding['generation']:(slice_dir/'cancel').touch()
        time.sleep(.02)
    if not (slice_dir/'done.json').exists():raise RuntimeError('slice did not stop')
    time.sleep(1)
    # A normal close after instrument removal; never kill the owned renderer for routine completion.
    import ctypes
    cb=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p)
    @cb
    def close(hwnd,_):
        pid=ctypes.c_ulong();ctypes.windll.user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd),ctypes.byref(pid))
        if pid.value==p.pid:ctypes.windll.user32.PostMessageW(ctypes.c_void_p(hwnd),0x10,0,0)
        return True
    ctypes.windll.user32.EnumWindows(close,0);code=p.wait(timeout=10);(out/'run.json').write_text(json.dumps({'pid':p.pid,'exit_code':code,'seconds':time.monotonic()-start,'live_binding':binding['session_event_sha256'],'watch':json.loads((watch/'done.json').read_text())},indent=2));print('complete',code)
    if code:raise RuntimeError('owned fixture did not exit cleanly')
    subprocess.run([sys.executable,str(ROOT/'scripts/analyze_cpu_producer.py'),str(out),str(out/'producer.json')],check=True)
finally:
    if p and p.poll() is None:(watch/'cancel').touch();p.terminate();p.wait(timeout=5)
    shutil.copy2(ROOT/'build/record-pack-control/Tests.original.exe',exe)
