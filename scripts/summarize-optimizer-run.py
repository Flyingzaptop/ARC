"""Summarize complete owned-renderer measurements without discarding slow frames.

GPU labels overlap; they are never added together. CPU preparation is a host
wall-time proxy, not total CPU execution across all threads.
"""
import argparse
import csv
import io
import json
import math
import statistics
from pathlib import Path


def quantile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lo = int(position)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)


def summarize(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    rows = [json.loads(line) for line in (directory / 'frames.jsonl').read_text().splitlines()]
    if not rows or not manifest.get('measurement_complete'):
        raise ValueError(f'Incomplete measurement: {directory}')
    times = [r['frame_ms'] for r in rows]
    if any(not math.isfinite(t) or t <= 0 for t in times):
        raise ValueError('Invalid frame timing; do not silently remove it')
    mean = statistics.mean
    poses = {}
    for row in rows:
        poses.setdefault(row['pose'], []).append(row['frame_ms'])
    proxy = [r['loop_ms'] - r['present_ms'] - r['swapchain_wait_ms'] - r['allocator_wait_ms'] for r in rows]
    result = dict(directory=str(directory), manifest=manifest, frames=len(rows),
                  mean_fps=1000 / mean(times), mean_frame_ms=mean(times),
                  p50_ms=quantile(times, .5), p95_ms=quantile(times, .95), p99_ms=quantile(times, .99),
                  one_percent_low_fps=1000 / mean(sorted(times, reverse=True)[:max(1, math.ceil(len(times) * .01))]),
                  one_percent_low_definition='1000 / mean of slowest ceil(N*0.01) frame times',
                  foreground_fraction=mean(r.get('foreground', False) for r in rows),
                  pose_coverage=len(poses), phase_balanced_fps=1000 / mean(mean(v) for v in poses.values()),
                  gpu_span_mean_ms=mean(r['gpu_span_ms'] for r in rows),
                  cpu_preparation_exact_ms=None,
                  host_loop_excluding_known_waits_proxy_ms=mean(proxy),
                  cpu_proxy_negative_frames=sum(v < 0 for v in proxy),
                  host_rm_executes_mean_ms=mean(r['cpu_ms'].get('RM Executes', 0) for r in rows),
                  wait_mean_ms=mean(r['swapchain_wait_ms'] for r in rows),
                  gpu_pass_mean_ms={k:mean(r['gpu_ms'].get(k, 0) for r in rows) for k in rows[0]['gpu_ms']},
                  gpu_passes_overlap_do_not_sum=True)
    samples = directory / 'gpu-samples.json'
    gpu = []
    if samples.exists():
        for sample in json.loads(samples.read_text()):
            if sample.get('measured_phase') and sample.get('exit_code') == 0:
                for values in csv.DictReader(io.StringIO(sample['csv'])):
                    gpu.append({k.strip():v.strip() for k,v in values.items()})
    result['gpu_telemetry'] = {}
    for key in ('power.draw [W]', 'enforced.power.limit [W]', 'temperature.gpu', 'clocks.current.graphics [MHz]', 'clocks.current.memory [MHz]', 'memory.used [MiB]'):
        values = []
        for row in gpu:
            try: values.append(float(row[key]))
            except (KeyError, ValueError): pass
        result['gpu_telemetry'][key] = dict(mean=mean(values),minimum=min(values),maximum=max(values),samples=len(values)) if values else None
    events = directory / 'automatic/events.jsonl'
    decisions = []
    if events.exists():
        for line in events.read_text().splitlines():
            event=json.loads(line)
            if event.get('phase')=='trial_finished': decisions.append(event)
    result['decisions']=decisions
    result['accepted_trials']=sum(d.get('decision_reason')=='accepted_net_gain_and_quality' for d in decisions)
    result['performance_eligible']=manifest.get('performance_run',True) and result['foreground_fraction']==1
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('runs',nargs='+',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    results=[summarize(p.resolve()) for p in args.runs]
    args.output.write_text(json.dumps(results,indent=2),encoding='utf-8')
    for r in results:
        print(f"{Path(r['directory']).name}: {r['mean_fps']:.3f} FPS; p95 {r['p95_ms']:.3f}ms; 1% low {r['one_percent_low_fps']:.3f}; GPU {r['gpu_span_mean_ms']:.3f}ms")
