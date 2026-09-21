"""Keep every frame, separate hardware regimes, and report validation coverage."""
import argparse,csv,io,json,math,statistics
from pathlib import Path

def percentile(values,q):
    if not values:return None
    v=sorted(values);at=(len(v)-1)*q;a=int(at);b=min(a+1,len(v)-1)
    return v[a]+(v[b]-v[a])*(at-a)

def frame_summary(rows):
    times=[r['frame_ms'] for r in rows]
    if not times or any(not math.isfinite(v) or v<=0 for v in times):raise ValueError('Invalid frame timing')
    slow=sorted(times)[-max(1,math.ceil(len(times)*.01)):]
    result=dict(frames=len(times),fps=1000*len(times)/sum(times),mean_ms=statistics.mean(times),p50_ms=percentile(times,.5),p95_ms=percentile(times,.95),p99_ms=percentile(times,.99),one_percent_low_fps=1000/statistics.mean(slow),max_ms=max(times),foreground_fraction=statistics.mean(bool(r.get('foreground')) for r in rows))
    for bound in (50,100):
        long=[v for v in times if v>bound];result[f'over_{bound}_count']=len(long);result[f'over_{bound}_sum_ms']=sum(long)
    for key in ('loop_ms','present_ms','submit_ms','swapchain_wait_ms','allocator_wait_ms','gpu_span_ms'):
        values=[r[key] for r in rows if key in r and math.isfinite(r[key]) and r[key]>=0]
        result[key]=dict(mean=statistics.mean(values),p50=percentile(values,.5),p99=percentile(values,.99),samples=len(values)) if values else None
    # A wall-time preparation proxy, not thread Running time or summed core time.
    preparation=[max(0,r['loop_ms']-r['swapchain_wait_ms']-r['allocator_wait_ms']) for r in rows]
    result['cpu_loop_excluding_explicit_waits_ms']=dict(mean=statistics.mean(preparation),p50=percentile(preparation,.5),p99=percentile(preparation,.99))
    result['gpu_pass_mean_ms']={key:statistics.mean(r.get('gpu_ms',{}).get(key,0) for r in rows) for key in sorted({k for r in rows for k in r.get('gpu_ms',{})})}
    return result

def json_lines(path):
    return [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()] if path.exists() else []

def summarize(case):
    manifest=json.loads((case/'manifest.json').read_text(encoding='utf-8'))
    rows=json_lines(case/'frames.jsonl');init=json_lines(case/'initialization-frames.jsonl')
    result=dict(case=str(case),manifest=manifest,measurement=frame_summary(rows),initialization=frame_summary(init) if init else None)
    if init:result['whole_ready_scene']=frame_summary(init+rows)
    scene_frames=rows[-1]['scene_frame']+1
    result['ready_frame_throughput_including_process_startup_shutdown_fps']=scene_frames/manifest['process_seconds']
    samples=[]
    for record in json_lines(case/'gpu-samples.jsonl'):
        if record.get('measured_phase') and record.get('exit_code')==0:
            samples.extend({k.strip():v.strip() for k,v in row.items()} for row in csv.DictReader(io.StringIO(record['csv'])))
    hardware={}
    for field in sorted({k for sample in samples for k in sample}):
        if field=='timestamp':continue
        if 'reasons' in field:hardware[field]=sorted({sample[field] for sample in samples if field in sample});continue
        values=[]
        for sample in samples:
            try:values.append(float(sample[field]))
            except (KeyError,ValueError):pass
        if values:hardware[field]=dict(min=min(values),max=max(values),mean=statistics.mean(values))
    caps=sorted({float(sample['enforced.power.limit [W]']) for sample in samples if sample.get('enforced.power.limit [W]','N/A')!='N/A'})
    result['hardware']=hardware;result['power_caps_w']=caps
    reasons=[]
    if len(caps)!=1:reasons.append('missing_or_mixed_power_limit')
    if not manifest.get('performance_run'):reasons.append('diagnostic_or_quality_run')
    if not manifest.get('measurement_complete'):reasons.append('incomplete_measurement')
    if not manifest.get('measurement_phase',{}).get('comparable'):reasons.append('initialization_phase_not_comparable')
    if result['measurement']['foreground_fraction']<.99:reasons.append('foreground_not_held')
    vendor=list(case.glob('*-perf-*.json'))
    result['render_resolution']=json.loads(vendor[0].read_text(encoding='utf-8')).get('RenderResolution') if len(vendor)==1 else None
    if result['render_resolution']!=[1920,1080]:reasons.append('native_resolution_unconfirmed')
    result['comparison_exclusions']=reasons
    events=json_lines(case/'automatic/events.jsonl')
    trials=[event for event in events if event.get('phase')=='trial_finished']
    accepted=[event for event in trials if event.get('decision_reason')=='accepted_net_gain_and_quality']
    telemetry=[event for event in events if event.get('phase')=='telemetry']
    result['automatic']=dict(trials=len(trials),accepted_checks=len(accepted),first_acceptance_seconds=accepted[0]['elapsed_ms']/1000 if accepted else None,quality_revocations=sum(e.get('phase')=='quality_revoked' for e in events),restoration_confirmed=events[-1].get('restoration_confirmed') if events else None,accepted_submission_time_fraction=events[-1].get('accepted_policy_submission_time_fraction') if events else None,activity_scope='CPU submission of still-valid accepted controls; not per-pixel GPU effect',quality_worker_launches=max((e.get('quality_worker_launches',0) for e in trials),default=0),worst_trial_quality={key:max((e['quality'][key] for e in trials if isinstance(e.get('quality',{}).get(key),(int,float))),default=None) for key in ('mean_linear_rgb_error','p99_tile_linear_rgb_error','worst_tile_linear_rgb_error')})
    memory={}
    for key in ('arc_control_default_heap_bytes','arc_control_upload_heap_bytes','arc_control_readback_heap_bytes','arc_image_default_heap_bytes','arc_image_readback_heap_bytes'):
        memory[key]=max((e.get(key,0) for e in telemetry),default=0)
    memory['critic_sampled_peak_private_bytes']=max((e.get('arc_children_sampled_peak_private_bytes',[0,0,0])[2] for e in telemetry),default=0)
    memory['compiler_sampled_peak_private_bytes']=max((e.get('arc_children_sampled_peak_private_bytes',[0,0,0])[1] for e in telemetry),default=0)
    memory['coverage']='Tracked own D3D allocations and child private commit; parent allocator/driver PSO storage not fully attributed'
    result['arc_memory']=memory
    return result

def main():
    parser=argparse.ArgumentParser();parser.add_argument('matrix',type=Path);args=parser.parse_args();root=args.matrix.resolve()
    cases=sorted(p for p in root.iterdir() if p.is_dir() and (p/'manifest.json').exists())
    runs=[summarize(case) for case in cases];groups={}
    for run in runs:
        if run['comparison_exclusions']:continue
        mode=Path(run['case']).name.split('-')[-1];cap=run['power_caps_w'][0];groups.setdefault((mode,cap),[]).append(run)
    aggregates=[]
    for (mode,cap),entries in sorted(groups.items()):
        fps=[r['measurement']['fps'] for r in entries]
        aggregates.append(dict(mode=mode,power_limit_w=cap,runs=len(entries),mean_fps=statistics.mean(fps),run_fps_stddev=statistics.stdev(fps) if len(fps)>1 else None,mean_p99_ms=statistics.mean(r['measurement']['p99_ms'] for r in entries),mean_one_percent_low_fps=statistics.mean(r['measurement']['one_percent_low_fps'] for r in entries)))
    result=dict(schema=1,runs=runs,aggregates=aggregates,excluded_frames=0)
    (root/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    lines=['| Mode | Cap W | Runs | FPS | p99 ms | 1% low FPS |','|---|---:|---:|---:|---:|---:|']
    for a in aggregates:lines.append(f"| {a['mode']} | {a['power_limit_w']:.0f} | {a['runs']} | {a['mean_fps']:.2f} | {a['mean_p99_ms']:.2f} | {a['mean_one_percent_low_fps']:.2f} |")
    lines+=['','All long frames retained. Hardware/context exclusions apply to entire runs.']
    for run in runs:
        if run['comparison_exclusions']:lines.append(f"- {Path(run['case']).name}: {', '.join(run['comparison_exclusions'])}")
    (root/'summary.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(json.dumps(dict(runs=len(runs),aggregates=aggregates)))

if __name__=='__main__':main()
