"""Report owned-host CPU/GPU timing without adding nested timers or hiding tails."""
import argparse
import csv
import io
import json
import math
import statistics as st
from pathlib import Path


def quantile(values, q):
    values = sorted(values)
    x = (len(values)-1)*q
    lo = math.floor(x)
    return values[lo] + (values[math.ceil(x)]-values[lo])*(x-lo)


def distribution(values):
    if not values or any(not math.isfinite(x) or x < 0 for x in values):
        raise ValueError("Missing or invalid measurements")
    return dict(mean=st.mean(values), p50=quantile(values,.5),
                p95=quantile(values,.95), p99=quantile(values,.99), maximum=max(values))


def load(path):
    manifest=json.loads((path/'manifest.json').read_text())
    rows=[json.loads(line) for line in (path/'frames.jsonl').read_text().splitlines()]
    if not rows or manifest.get('exit_code') != 0:
        raise ValueError(f"Unfinished run: {path}")
    vendor=json.loads(next(path.glob('*-perf-*.json')).read_text())
    frame=[r['frame_ms'] for r in rows]
    result={'path':str(path.resolve()),'manifest':manifest,'actual_resolution':vendor['RenderResolution'],
            'frames':len(rows),'foreground_fraction':st.mean(r['foreground'] for r in rows),
            'fps':1000/st.mean(frame),'one_percent_low_fps':1000/st.mean(sorted(frame)[-math.ceil(len(frame)*.01):]),
            'frame_ms':distribution(frame),'gpu_span_ms':distribution([r['gpu_span_ms'] for r in rows]),
            'long_frames':{str(t):{'count':sum(x>t for x in frame),'total_ms':sum(x for x in frame if x>t)} for t in (50,100)},
            'timers':{},'hardware':{}}
    for group in ('cpu_ms','gpu_ms'):
        keys=sorted(set().union(*(r[group] for r in rows)))
        result[group]={k:{**distribution([r[group][k] for r in rows if k in r[group]]),
                         'observations':sum(k in r[group] for r in rows)} for k in keys}
    for k in ('loop_ms','present_ms','submit_ms','swapchain_wait_ms','allocator_wait_ms'):
        result['timers'][k]=distribution([r[k] for r in rows])
    result['timers']['outside_loop_ms']=distribution([max(0,r['frame_ms']-r['loop_ms']) for r in rows])
    # Equal weight for every observed route pose; this is a separate view, never
    # a replacement for full-session cadence or its slow frames.
    poses={p:[r for r in rows if r['pose']==p] for p in sorted({r['pose'] for r in rows})}
    result['pose_balanced_gpu_ms']={k:st.mean(st.mean(r['gpu_ms'][k] for r in rs if k in r['gpu_ms'])
        for rs in poses.values() if any(k in r['gpu_ms'] for r in rs)) for k in result['gpu_ms']}
    hardware=[]
    for sample in json.loads((path/'gpu-samples.json').read_text()):
        if sample['measured_phase'] and sample['exit_code']==0:
            hardware.extend({k.strip():v.strip() for k,v in r.items()} for r in csv.DictReader(io.StringIO(sample['csv'])))
    for k in (hardware[0] if hardware else []):
        try: result['hardware'][k]=distribution([float(r[k]) for r in hardware])
        except ValueError: result['hardware'][k]=sorted({r[k] for r in hardware})
    if (path/'arc.json').exists():
        result['arc']=json.loads((path/'arc.json').read_text())
    return result


def main():
    p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('runs',nargs='+',type=Path)
    a=p.parse_args();runs={r.name:load(r) for r in a.runs}
    if len({r['manifest']['host_sha256'] for r in runs.values()})!=1:
        raise ValueError('Different host binaries')
    if len({tuple(r['actual_resolution']) for r in runs.values()})!=1:
        raise ValueError('Different render resolutions')
    report={'runs':runs,'limitations':[
        'CPU SDK scopes refer to cpu_profiler_frame (previous frame); they are not aligned by row with current CPU wall timers.',
        'Nested CPU/GPU scopes overlap and must not be summed. Begin Frame includes waiting; EndFrame includes submit/present.',
        'GPU timestamp span is elapsed GPU timeline duration, not utilization; CPU starvation can leave gaps.',
        'No whole-process CPU running-time/critical-path attribution is available in host scope timers.',
        'No image-quality equivalence claim; target feedback permits visual degradation.',
        'GPU enforced power limit is recorded separately from measured power draw.']}
    a.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    lines=['# CPU/GPU render breakdown','',*report['limitations'],'']
    names=list(runs)
    lines+=['| Metric (ms unless FPS) | '+' | '.join(names)+' |','|---|'+'---:|'*len(names)]
    def row(label,values): lines.append('| '+label+' | '+' | '.join(f'{v:.4f}' for v in values)+' |')
    row('FPS',[r['fps'] for r in runs.values()]);row('1% low FPS',[r['one_percent_low_fps'] for r in runs.values()])
    for key in ('frame_ms','gpu_span_ms'):
        for metric in ('mean','p50','p95','p99'):row(key+' '+metric,[r[key][metric] for r in runs.values()])
    for group in ('timers','cpu_ms','gpu_ms'):
        for key in sorted(set().union(*(r[group] for r in runs.values()))):
            row(group+' / '+key,[r[group].get(key,{'mean':float('nan')})['mean'] for r in runs.values()])
    a.output.with_suffix('.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(a.output)

if __name__=='__main__':main()
