"""Analyze complete lab frame CSVs; never infer display FPS from Present cadence."""
import argparse,csv,json,pathlib,statistics,hashlib

def quantile(values,p):
 a=sorted(values);x=(len(a)-1)*p;i=int(x);return a[i]+(a[min(i+1,len(a)-1)]-a[i])*(x-i)
def analyze(folder):
 runs=[]
 for metadata in sorted(folder.glob('matrix-*.json')):
  if metadata.name.endswith('.ir.json') or '.analysis.' in metadata.name or '.meta.' in metadata.name:continue
  data=json.loads(metadata.read_text());base=metadata.with_suffix('');csvfile=base.with_suffix('.csv')
  if 'mode' not in data or not csvfile.exists():continue
  if data.get('returncode')!=0:runs.append(dict(run=base.name,mode=data['mode'],verdict='INVALID',failure=data));continue
  rows=[r for r in csv.DictReader(csvfile.open()) if r['warmup']=='0'];result=dict(run=base.name,mode=data['mode'],frames=len(rows),display_fps_measured=False)
  for key in ['cpu_submission_ms','present_return_ms','frame_ms','gpu_ms']:
   values=[float(r[key]) for r in rows];result[key]=dict(mean=statistics.mean(values),p50=quantile(values,.5),p95=quantile(values,.95),p99=quantile(values,.99))
  result['present_hz']=1000/result['frame_ms']['mean']
  image=pathlib.Path(str(base)+'.rgba8');result['image_sha256']=hashlib.sha256(image.read_bytes()).hexdigest()
  irpath=pathlib.Path(str(base)+'.ir.json')
  if irpath.exists():
   ir=json.loads(irpath.read_text());result['ir']={k:ir.get(k) for k in ['total_work','dropped','incomplete','history_truncated','accepted_actions','rejected_actions']};result['retained_work']=len(ir['work']);result['coverage_counts']={}
   for w in ir['work']:
    if w.get('coverage'):result['coverage_counts'][w['coverage']]=result['coverage_counts'].get(w['coverage'],0)+1
  runs.append(result)
 good=[r for r in runs if r.get('frames')];groups={m:[r for r in good if r['mode']==m] for m in ['native','passthrough','observe','optimize']};summary={}
 baseline=statistics.mean(r['cpu_submission_ms']['mean'] for r in groups['native']) if groups['native'] else None
 for mode,rows in groups.items():
  if not rows:continue
  cpu=statistics.mean(r['cpu_submission_ms']['mean'] for r in rows);summary[mode]=dict(runs=len(rows),mean_cpu_submission_ms=cpu,mean_gpu_ms=statistics.mean(r['gpu_ms']['mean'] for r in rows),mean_frame_ms=statistics.mean(r['frame_ms']['mean'] for r in rows))
  if baseline:summary[mode].update(cpu_delta_ms=cpu-baseline,cpu_delta_percent=(cpu/baseline-1)*100,diagnostic_two_percent_pass=cpu<=baseline*1.02)
 return dict(scope='owned redundant-clear fixture; not independent-engine acceleration or heavy-workload gate',runs=runs,summary=summary,all_images_equal=len({r['image_sha256'] for r in good})==1)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('folder',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);a=p.parse_args();r=analyze(a.folder);a.output.write_text(json.dumps(r,indent=2));print(json.dumps(r['summary'],indent=2))
