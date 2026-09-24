"""Source-frame joined timing analysis. No smoothing or outlier removal."""
import argparse,csv,json,statistics,hashlib,html
from pathlib import Path
def stat(xs):
 v=sorted(xs)
 if not v:return None
 def q(p):
  f=(len(v)-1)*p;i=int(f);return v[i]+(v[min(i+1,len(v)-1)]-v[i])*(f-i)
 return dict(count=len(v),mean=statistics.mean(v),median=statistics.median(v),p95=q(.95),p99=q(.99),max=max(v))
def wicked(run):
 m=json.loads((run/'manifest.json').read_text());frames={}
 with (run/'cpu.csv').open() as f:rows=list(csv.DictReader(f))
 for row in rows:
  f=frames.setdefault(int(row['frame']),{});f[row['event']]=float(row['ms'])
  if row['event']=='CPU processing through submission ms':f['time_s']=float(row['elapsed_ms'])/1000
 result=[]
 for key,f in sorted(frames.items()):
  if not m['warmup_seconds']<=f.get('time_s',-1)<m['warmup_seconds']+m['measurement_seconds']:continue
  result.append(dict(frame_id=key,time_s=f['time_s']-m['warmup_seconds'],cpu_ms=f.get('CPU processing through submission ms'),gpu_ms=f.get('Retired GPU frame span ms'),present_ms=f.get('Controlled Present interval'),offload_gpu_ms=f.get('Resident retired GPU work ms'),build_items=f.get('Resident previous build items skipped'),sort_items=f.get('Resident previous sort items skipped'),pack_items=f.get('Resident previous pack items skipped'),offload_permission=f.get('Resident permission'),certificate_valid=f.get('Resident certificate valid'),application_update_ms=f.get('Application Update'),application_render_ms=f.get('Application Render'),offload_prepare_ms=f.get('Resident previous prepare ms'),offload_record_ms=f.get('Resident previous GPU record ms')))
 return result,m
def cauldron(run):
 d=run/'session';m=json.loads((d/'manifest.json').read_text());rows=[json.loads(l) for p in [d/'initialization-frames.jsonl',d/'frames.jsonl'] for l in p.read_text().splitlines()];gpu={x['gpu_source_frame']:x['gpu_span_ms'] for x in rows if x.get('gpu_source_frame',2**64-1)!=2**64-1}
 return [dict(frame_id=x['scene_frame'],pose=x['pose'],time_s=x['measurement_elapsed_ms']/1000,cpu_ms=x['cpu_through_submission_ms'],gpu_ms=gpu.get(x['scene_frame']),present_ms=x['present_interval_ms'],offload_gpu_ms=None,build_items=0,sort_items=0,pack_items=0,swapchain_wait_ms=x.get('swapchain_wait_ms'),allocator_wait_ms=x.get('allocator_wait_ms'),submit_ms=x.get('submit_ms'),foreground=x.get('foreground')) for x in rows if x['phase']=='measurement'],m
def graph(path,series,key,title):
 colors=['#2563eb','#ea580c','#b91c1c','#0891b2'];allv=[r[key] for rows in series.values() for r in rows if r.get(key) is not None];ymax=max(allv)*1.03 if allv else 1;xmax=max(r['time_s'] for rows in series.values() for r in rows)
 svg=['<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="560"><rect width="1200" height="560" fill="white"/>',f'<text x="65" y="30" font-family="sans-serif" font-size="18">{html.escape(title)}</text>']
 for i in range(6):
  y=470-i*80;svg.append(f'<path d="M65 {y} H1180" stroke="#ddd"/><text x="5" y="{y}">{i*ymax/5:.1f}</text>')
 for i,(name,rows) in enumerate(series.items()):
  points=' '.join(f'{65+r["time_s"]/xmax*1115:.2f},{470-r[key]/ymax*400:.2f}' for r in rows if r.get(key) is not None)
  svg.append(f'<polyline points="{points}" fill="none" stroke="{colors[i%4]}" stroke-width=".8" opacity=".65"/><text x="{65+i*160}" y="525" fill="{colors[i%4]}">{name}</text>')
 svg.append(f'<text x="65" y="495">0 s</text><text x="1120" y="495">{xmax:.1f} s</text></svg>');path.write_text('\n'.join(svg))
def main():
 p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('--renderer',choices=['wicked','cauldron'],required=True);a=p.parse_args();series={};summary={}
 for name in ['A1','B1','B2','A2']:
  run=a.directory/name
  if not run.exists():continue
  rows,m=(wicked if a.renderer=='wicked' else cauldron)(run)
  for r in rows:r['present_rate_hz']=1000/r['present_ms'] if r.get('present_ms',0) and r['present_ms']>0 else None
  session=run if a.renderer=='wicked' else run/'session'
  decision_file=session/'automatic/events.jsonl'
  original_policy=not decision_file.exists()
  if decision_file.exists():
   events=[json.loads(l) for l in decision_file.read_text().splitlines()]
   original_policy=bool(events) and all(e.get('accepted_policy_submission_frames',0)==0 and not e.get('active_action') and not e.get('vrs_requested') and not e.get('pixel_mip_half_steps') and not e.get('pixel_comparison_taps') and e.get('pixel_independent_sample_percent',100)==100 for e in events)
  for row in rows:
   row['gpu_quality_policy']='original; no applied policy reported across session' if original_policy else 'see decision journal; exact per-frame mapping unavailable'
   if a.renderer=='cauldron':row['cpu_offload_state']='unsupported'
   elif not m.get('full_arc'):row['cpu_offload_state']='disabled_no_dll'
   elif not m.get('host_cpu_offload'):row['cpu_offload_state']='disabled_configuration'
   elif row.get('offload_permission')==0:row['cpu_offload_state']='session_disabled'
   elif row.get('certificate_valid')==0:row['cpu_offload_state']='unsupported_scene_certificate'
   elif row.get('build_items',0):row['cpu_offload_state']='active_supported'
   elif row.get('offload_permission') is None:row['cpu_offload_state']='telemetry_tail_missing'
   else:row['cpu_offload_state']='original_other_admission_guard'
  series[name]=rows
  with (run/'joined-frames.csv').open('w',newline='') as f:
   writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
  summary[name]={k:stat([r[k] for r in rows if r.get(k) is not None]) for k in ['cpu_ms','gpu_ms','present_ms','present_rate_hz','offload_gpu_ms','build_items','sort_items','pack_items','application_update_ms','application_render_ms','offload_prepare_ms','offload_record_ms','swapchain_wait_ms','allocator_wait_ms','submit_ms']}
  periods=[r['present_ms'] for r in rows if r.get('present_ms',0) and r['present_ms']>0];summary[name]['aggregate_present_rate_hz']=1000/statistics.mean(periods)
  summary[name]['gpu_missing_frames']=sum(r.get('gpu_ms') is None for r in rows)
 for key,title in [('cpu_ms','CPU processing through submission (ms)'),('gpu_ms','GPU graphics span including offload (ms)'),('present_rate_hz','Successful Present return frequency (Hz), NOT displayed FPS')]:graph(a.directory/(key+'.svg'),series,key,a.renderer+': '+title)
 result={'renderer':a.renderer,'displayed_fps_measured':False,'outliers_removed':False,'gpu_join':'source frame ID; missing retired tail remains missing','runs':summary};(a.directory/'comparison.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
if __name__=='__main__':main()
