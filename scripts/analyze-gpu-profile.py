"""Validate ARC-only cost discovery against independent host GPU intervals.

Engine labels are consumed here solely as an external test oracle. They are
neither requested by nor fed back into the generic probe.
"""
import argparse
from collections import Counter,defaultdict
import json
from pathlib import Path
import statistics
import numpy as np
from PIL import Image
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

parser=argparse.ArgumentParser();parser.add_argument("directory",type=Path)
root=parser.parse_args().directory.resolve()
names=["baseline-a","observe","profile-a","profile-b","baseline-b"]
rows={};manifests={};runs={}
for name in names:
    path=root/name
    manifest=json.loads((path/"manifest.json").read_text());manifests[name]=manifest
    data=[json.loads(line) for line in (path/"frames.jsonl").read_text().splitlines()];rows[name]=data
    if len(data)!=600 or [r['frame'] for r in data]!=list(range(600)):raise ValueError("Incomplete trajectory")
    if sorted(r['pose'] for r in data)!=list(range(600)):raise ValueError("Unmatched camera trajectory")
    if any('FPSLimiter' in r['gpu_ms'] for r in data):raise ValueError("Artificial GPU limiter")
    vendor=json.loads(next(path.glob('*-perf-*.json')).read_text())
    if vendor['RenderResolution']!=[1920,1080]:raise ValueError("Non-native render resolution")
    periods=np.array([r['frame_ms'] for r in data])
    if np.any(periods<=0) or not np.isfinite(periods).all():raise ValueError("Invalid frame timing")
    runs[name]={"fps":float(1000/periods.mean()),"p99_ms":float(np.quantile(periods,.99)),
                "one_percent_low_fps":float(1000/np.sort(periods)[-6:].mean()),
                "cpu_record_mean_ms":float(np.mean([r['cpu_ms']['RM Executes'] for r in data])),
                "first_16_frame_ms":float(periods[:16].mean()),"first_16_cpu_record_ms":float(np.mean([r['cpu_ms']['RM Executes'] for r in data[:16]]))}
    if manifest['dll_sha256']:
        arc=json.loads((path/'arc.json').read_text())
        if arc['quality_mutations'] or arc['present_failures'] or arc['gpu_profile']['faults']:raise ValueError("Non-observational or unhealthy run")
        if arc['gpu_profile']['retained_gpu_recordings'] or arc['gpu_profile']['tracked_recordings']:raise ValueError("Unretired capture resources")
if len({m['host_sha256'] for m in manifests.values()})!=1:raise ValueError("Different host binaries")
if len({m['dll_sha256'] for m in manifests.values() if m['dll_sha256']})!=1:raise ValueError("Different probe binaries")

def overlap(a,b):return max(0,min(a[1],b[1])-max(a[0],b[0]))
def union_duration(intervals):
    total=0;end=0
    for a,b in sorted(intervals):
        total+=max(0,b-max(a,end));end=max(end,b)
    return total

profiles={}
for name in ['profile-a','profile-b']:
    profile=json.loads((root/name/'gpu-profile.json').read_text())
    for field in ['faults','pending_gpu_jobs','capacity_declines','dropped_intervals','cached_replay_samples_dropped','unsupported_segments']:
        if profile[field]:raise ValueError(f"Incomplete {name}: {field}={profile[field]}")
    if profile['present_windows']!=16 or profile['timed_out'] or profile['engine_labels_used'] or profile['shader_mutations'] or profile['image_readbacks']:
        raise ValueError("Invalid capture contract")
    intervals=profile['intervals'];pipelines={p['id']:p for p in profile['pipelines']}
    if len({r['queue'] for r in intervals})!=1:raise ValueError("This host comparison expects one queue; do not sum concurrent queues")
    if any(r['frequency']!=1_000_000_000 for r in intervals):raise ValueError("Oracle conversion assumes this host's 1 GHz GPU timestamps")
    grouped=defaultdict(list)
    for span in intervals:grouped[span['frame']].append(span)
    if set(grouped)!=set(range(16)):raise ValueError("Missing Present epochs")
    categories=Counter();kernels=defaultdict(list);covered=[];frame_matches=[];oracle=[]
    for epoch,spans in grouped.items():
        low=min(r['begin_ticks'] for r in spans);high=max(r['end_ticks'] for r in spans)
        # Match by GPU time, not buffered CPU frame index or engine label.
        host=max(rows[name],key=lambda r:overlap((low,high),(r['gpu_begin_ns'],r['gpu_end_ns'])))
        host_span=(host['gpu_begin_ns'],host['gpu_end_ns'])
        fraction=overlap((low,high),host_span)/(high-low)
        if fraction<.98:raise ValueError("GPU timelines do not align")
        covered.append(union_duration([(r['begin_ticks'],r['end_ticks']) for r in spans])/(host_span[1]-host_span[0]))
        frame_matches.append({'present_epoch':epoch,'host_buffered_row':host['frame'],'timeline_overlap_fraction':fraction})
        for span in spans:
            categories[span['kind']]+=span['gpu_ms']/16
            if span['kind']=='compute':
                p=pipelines.get(span['pipeline'])
                if not p or not p['sha256']:raise ValueError("Unidentified compute shader")
                kernels[p['sha256']].append(span)
            else:continue
            a=(span['begin_ticks'],span['end_ticks'])
            best=None
            for label,region in host['gpu_intervals'].items():
                b=(region['begin_ns'],region['end_ns']);common=overlap(a,b);union=(a[1]-a[0])+(b[1]-b[0])-common
                score=common/union if union else 0
                if best is None or score>best[0]:best=(score,label,(b[1]-b[0])/1e6)
            oracle.append({'shader':p['sha256'],'arc_ms':span['gpu_ms'],'host_label':best[1],'interval_iou':best[0],'host_ms':best[2]})
    ranking=[]
    for sha,spans in kernels.items():
        p=pipelines[spans[0]['pipeline']];matches=[r for r in oracle if r['shader']==sha]
        direct={tuple(s['dispatch']) for s in spans if not s['indirect_calls'] and not s['mixed_dispatch']}
        ranking.append({'sha256':sha,'mean_ms_per_window':sum(s['gpu_ms'] for s in spans)/16,'interval_count':len(spans),
                        'threads':p['threads'],'direct_dispatches':[list(d) for d in sorted(direct)],
                        'indirect_api_calls':sum(s['indirect_calls'] for s in spans),'declared_binding_count':len(p['declared_bindings']),
                        'reflection_available':p['reflection_available'],'unbounded_bindings':p['unbounded_bindings'],
                        'requires_flags':p['requires_flags'],'instruction_metadata':p['instructions'],
                        'validation_only_host_labels':dict(Counter(r['host_label'] for r in matches)),
                        'validation_only_mean_interval_iou':statistics.mean(r['interval_iou'] for r in matches)})
    ranking.sort(key=lambda k:k['mean_ms_per_window'],reverse=True)
    if ranking[0]['validation_only_mean_interval_iou']<.95:raise ValueError("Dominant compute interval disagrees with independent instrumentation")
    profiles[name]={'interval_count':len(intervals),'compute_shaders':len(ranking),'mean_captured_gpu_ms':sum(categories.values()),
                    'mean_frame_timeline_coverage':statistics.mean(covered),'categories_ms':dict(categories),'top_compute':ranking[:12],
                    'frame_alignment':frame_matches,'indirect_api_calls':sum(s['indirect_calls'] for s in intervals),
                    'captured_timestamp_bytes':len(intervals)*16,'raw_counters':{k:v for k,v in profile.items() if k not in ['intervals','pipelines']}}
if profiles['profile-a']['top_compute'][0]['sha256']!=profiles['profile-b']['top_compute'][0]['sha256']:raise ValueError("Dominant shader fingerprint changed")

def image_array(name):return np.asarray(Image.open(next((root/name).glob('*.png'))).convert('RGB'),dtype=np.float32)/255
reference=image_array('baseline-a');repeat=image_array('baseline-b')
def image_error(a,b):
    d=abs(a**2.2-b**2.2);return {'mean_linear':float(d.mean()),'peak_linear':float(d.max())}
baseline_ms=(1000/runs['baseline-a']['fps']+1000/runs['baseline-b']['fps'])/2
top=profiles['profile-a']['top_compute'][0]['mean_ms_per_window'];total=profiles['profile-a']['mean_captured_gpu_ms']
summary={'schema':1,'runs':runs,'profiles':profiles,'baseline_combined_fps':1000/baseline_ms,
         'baseline_repeat_drift_fraction':abs(runs['baseline-a']['fps']-runs['baseline-b']['fps'])/min(runs['baseline-a']['fps'],runs['baseline-b']['fps']),
         'repeat_image_error':image_error(reference,repeat),'profile_image_error':image_error(reference,image_array('profile-a')),
         'single_top_kernel_zero_cost_upper_bound':total/(total-top),'hypothetical_savings_for_2x_ms':total/2,
         'optimization_gain_claim':False,'resource_contracts_proven':False,'shader_or_quality_changes':False}
(root/'analysis.json').write_text(json.dumps(summary,indent=2))
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10})
fig,axes=plt.subplots(1,2,figsize=(12,4.6),layout='constrained')
rank=profiles['profile-a']['top_compute'][:6]
axes[0].barh(range(len(rank)),[r['mean_ms_per_window'] for r in rank],color='#559e9d')
axes[0].set_yticks(range(len(rank)),[r['sha256'][:10] for r in rank]);axes[0].invert_yaxis();axes[0].set_xlabel('GPU, мс на кадр захвата');axes[0].set_title('Compute-шейдеры, найденные ARC по DX12')
cats=profiles['profile-a']['categories_ms'];labels={'raster_depth':'Отрисовка в глубину','raster_color':'Цветовая отрисовка','compute':'Compute','opaque':'Не классифицировано','raster_unknown':'Прочая отрисовка'}
axes[1].bar([labels[k] for k in cats],list(cats.values()),color='#7e929f');axes[1].set_ylabel('GPU, мс');axes[1].set_title('Разделение затрат без названий проходов');axes[1].tick_params(axis='x',labelrotation=15)
fig.savefig(root/'profile-costs.png',dpi=160);plt.close(fig)
print(json.dumps({'profiles':{n:{k:p[k] for k in ['interval_count','compute_shaders','mean_captured_gpu_ms','mean_frame_timeline_coverage','indirect_api_calls']} for n,p in profiles.items()},'top_shader':rank[0],'fps':{n:r['fps'] for n,r in runs.items()}},indent=2))
