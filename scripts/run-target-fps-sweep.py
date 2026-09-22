"""Seven sequential 60-second Cauldron runs, goals from baseline median FPS."""
import argparse,csv,hashlib,io,json,math,statistics,subprocess,sys,shutil
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--sdk',type=Path,required=True);p.add_argument('--compiler',type=Path,required=True);p.add_argument('--cache-seed',type=Path,required=True);a=p.parse_args()
repo=Path(__file__).resolve().parents[1];a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=False)
seed=a.output/'cache-seed';shutil.copytree(a.cache_seed,seed)
results=[]
def run(label,multiplier=None,target=None):
 out=a.output/label
 cmd=[sys.executable,str(repo/'scripts/run-cauldron-benchmark.py'),str(a.sdk),str(out),'--measurement-seconds','60','--borderless','--visible']
 if target is not None:
  if not 0<target<=1000:raise ValueError(f'Target {target} exceeds runtime supported range; do not silently clamp')
  cache=a.output/(label+'-cache');shutil.copytree(seed,cache)
  cmd+=['--dll',str(repo/'build/Release/arc-dx12-probe.dll'),'--mode','compute-off','--auto-target',str(target),'--control-mode','target_feedback','--quality-profile','aggressive','--compiler',str(a.compiler),'--cache',str(cache)]
 print(json.dumps({'starting':label,'target_fps':target}),flush=True)
 subprocess.run(cmd,cwd=repo,check=True,timeout=120)
 manifest=json.loads((out/'manifest.json').read_text());assert manifest['measurement_complete'] and manifest['exit_code']==0
 rows=[json.loads(x) for x in (out/'frames.jsonl').read_text().splitlines()];fts=[r['frame_ms'] for r in rows];elapsed=sum(fts)
 def percentile(values,q):
  v=sorted(values);pos=(len(v)-1)*q;i=int(pos);return v[i]+(v[min(i+1,len(v)-1)]-v[i])*(pos-i)
 gpu=[]
 for sample in json.loads((out/'gpu-samples.json').read_text()):
  if not sample['measured_phase']:continue
  for row in csv.DictReader(io.StringIO(sample['csv'])):
   gpu.append({k.strip():v.strip() for k,v in row.items()})
 def readings(prefix):
  values=[]
  for row in gpu:
   for k,v in row.items():
    if k.startswith(prefix):
     try:values.append(float(v))
     except ValueError:pass
  return values
 power=readings('enforced.power.limit');temperatures=readings('temperature.gpu');clocks=readings('clocks.current.graphics')
 info={'label':label,'multiplier':multiplier,'target_fps':target,'frames':len(rows),'mean_fps':1000*len(rows)/elapsed,'median_fps':statistics.median([1000/x for x in fts]),'p99_frame_ms':percentile(fts,.99),'one_percent_low_fps':1000/statistics.mean(sorted(fts,reverse=True)[:max(1,math.ceil(len(fts)*.01))]),'over_50ms_frames':sum(x>50 for x in fts),'observed_seconds':elapsed/1000,'power_limit_w':sorted(set(power)),'temperature_min_max_c':[min(temperatures),max(temperatures)] if temperatures else None,'graphics_clock_min_max_mhz':[min(clocks),max(clocks)] if clocks else None,'foreground_fraction':sum(bool(r.get('foreground')) for r in rows)/len(rows),'dll_sha256':manifest['dll_sha256'],'host_sha256':manifest['host_sha256']}
 if target:
  info['time_at_or_above_target_percent']=100*sum(x for x in fts if x<=1000/target)/elapsed
  info['time_within_controller_budget_percent']=100*sum(x for x in fts if x<=1.03*1000/target)/elapsed
  events=[json.loads(x) for x in (out/'automatic/events.jsonl').read_text().splitlines()]
  assert events[-1].get('restoration_confirmed')
  assert not list((out/'automatic').rglob('quality.json'))
  info['last_policy']=next((r.get('targets') for r in reversed(events) if 'targets' in r),[])
  info['policy_changes']=sum(r.get('reason')=='reducing_measured_expensive_compute' for r in events)
  info['rollbacks']=sum(r.get('reason') in ('sequential_regression_reverted','no_modified_submission_reverted') for r in events)
  info['limited_observations']=sum(r.get('phase')=='limited' for r in events)
  info['restoration_confirmed']=True
 with (out/'fps.csv').open('w',newline='') as f:
  writer=csv.writer(f);writer.writerow(['elapsed_s','frame_ms','instantaneous_fps','gpu_span_ms'])
  for r in rows:writer.writerow([r['measurement_elapsed_ms']/1000,r['frame_ms'],1000/r['frame_ms'],r.get('gpu_span_ms')])
 results.append(info);(a.output/'summary.json').write_text(json.dumps(results,indent=2));print(json.dumps({'completed':info}),flush=True)
run('00-baseline')
baseline=results[0]['median_fps']
(a.output/'plan.json').write_text(json.dumps({'baseline_median_fps':baseline,'goals':[{'multiplier':m,'target_fps':m*baseline} for m in [1.25,1.5,2,2.5,5,10]],'measurement_seconds':60,'initialization_seconds':0,'native_warmup_frames':120,'route':'primary','cache':'identical_initial_seed_per_arc_run','quality_check':False},indent=2))
for i,m in enumerate([1.25,1.5,2,2.5,5,10],1):run(f'{i:02d}-x{m:g}',m,baseline*m)
print('SWEEP COMPLETE',flush=True)
