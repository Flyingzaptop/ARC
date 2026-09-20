"""Compare complete owned-renderer placement runs, retaining thermal caveats."""
import argparse
import json
import math
import statistics
from pathlib import Path


def quantile(values, q):
    values = sorted(values)
    position = (len(values)-1)*q
    low = math.floor(position)
    high = math.ceil(position)
    return values[low]*(high-position)+values[high]*(position-low) if low != high else values[low]


def analyze(root):
    runs, invalid = [], []
    for directory in sorted(p.parent for p in root.glob('*/manifest.json')):
        manifest = json.loads((directory/'manifest.json').read_text())
        if 'exit_code' not in manifest:
            invalid.append({'case': directory.name, 'reason': 'unfinished'})
            continue
        try:
            rows = [json.loads(line) for line in (directory/'frames.jsonl').read_text().splitlines()]
            expected = manifest['measured_frames']
            if manifest['exit_code'] or len(rows) != expected or not rows:
                raise ValueError('incomplete_or_failed_measurement')
            periods = [row['frame_ms'] for row in rows]
            if any(not math.isfinite(x) or x <= 0 for x in periods):
                raise ValueError('invalid_frame_period')
            arc = json.loads((directory/'arc.json').read_text())
            placement = arc['worker_placement']
            if placement['failures'] or placement.get('rollback_pending'):
                raise ValueError('placement_fault_or_pending_rollback')
            mode = manifest['worker_placement']
            mode_index = {'normal': 0, 'prefer': 1, 'core': 2, 'partition': 3}.get(mode)
            if mode_index is not None and not placement['presents_by_mode'][mode_index]:
                raise ValueError('requested_placement_not_exercised')
            gpu = []
            for sample in json.loads((directory/'gpu-samples.json').read_text()):
                if not sample.get('measured_phase') or sample.get('exit_code'):
                    continue
                lines = sample['csv'].splitlines()
                if len(lines) < 2:
                    continue
                fields = [x.strip() for x in lines[-1].split(',')]
                if len(fields) == 7 and float(fields[1]) >= 50:
                    gpu.append((float(fields[3]), float(fields[5]), float(fields[6])))
            signatures = [manifest.get(k) for k in ('host_sha256', 'dll_sha256', 'compiler_sha256', 'effective_mode', 'automatic_target_fps', 'measured_frames')]
            run = {'case': directory.name, 'mode': mode, 'signature': signatures, 'frames': len(rows),
                'fps': 1000/statistics.mean(periods), 'mean_frame_ms': statistics.mean(periods),
                'p95_ms': quantile(periods, .95), 'p99_ms': quantile(periods, .99), 'max_ms': max(periods),
                'frames_over_25ms': sum(x > 25 for x in periods),
                'render_prepare_ms': statistics.mean(row['cpu_ms']['RM Executes'] for row in rows),
                'submit_ms': statistics.mean(row['submit_ms'] for row in rows),
                'gpu_span_ms': statistics.mean(row['gpu_span_ms'] for row in rows),
                'trials': len(list((directory/'automatic').glob('trial-*/decision.json'))),
                'placement': placement, 'thermal_gate': bool(manifest.get('thermal_gate', {}).get('enabled'))}
            if gpu:
                run['gpu_clock_mhz'] = statistics.mean(x[0] for x in gpu)
                run['gpu_clock_spread_fraction'] = (max(x[0] for x in gpu)-min(x[0] for x in gpu))/run['gpu_clock_mhz']
                run['max_gpu_temperature_c'] = max(x[1] for x in gpu)
                run['mean_gpu_power_w'] = statistics.mean(x[2] for x in gpu)
            runs.append(run)
        except (KeyError, ValueError, OSError) as error:
            invalid.append({'case': directory.name, 'reason': str(error)})
    for index, run in enumerate(runs):
        if run['mode'] == 'normal':
            continue
        before = next((r for r in reversed(runs[:index]) if r['mode'] == 'normal' and r['signature'] == run['signature']), None)
        after = next((r for r in runs[index+1:] if r['mode'] == 'normal' and r['signature'] == run['signature']), None)
        if not before or not after:
            run['comparison_qualified'] = False
            continue
        baseline = (before['mean_frame_ms']+after['mean_frame_ms'])/2
        baseline_p99 = (before['p99_ms']+after['p99_ms'])/2
        run['references'] = [before['case'], after['case']]
        run['fps_change_percent'] = (baseline/run['mean_frame_ms']-1)*100
        run['p99_change_percent'] = (run['p99_ms']/baseline_p99-1)*100
        thermal = all(r['thermal_gate'] and r.get('max_gpu_temperature_c', 100) < 80 and r.get('gpu_clock_spread_fraction', 1) < .05 for r in (before, run, after))
        run['comparison_qualified'] = thermal and abs(before['mean_frame_ms']-after['mean_frame_ms']) <= baseline*.03
    modes = {}
    for mode in sorted({r['mode'] for r in runs}):
        selected = [r for r in runs if r['mode'] == mode]
        qualified = [r for r in selected if r.get('comparison_qualified')]
        modes[mode] = {'runs': len(selected), 'qualified_comparisons': len(qualified),
            'median_fps': statistics.median(r['fps'] for r in selected),
            'fps_range': [min(r['fps'] for r in selected), max(r['fps'] for r in selected)],
            'median_p99_ms': statistics.median(r['p99_ms'] for r in selected)}
        if qualified:
            modes[mode]['median_fps_change_percent'] = statistics.median(r['fps_change_percent'] for r in qualified)
            modes[mode]['median_p99_change_percent'] = statistics.median(r['p99_change_percent'] for r in qualified)
    return {'status': 'experimental_comparison', 'not_game_logic_parallelization': True,
            'render_prepare_is_not_total_cpu_frametime': True, 'modes': modes, 'runs': runs, 'invalid': invalid}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = analyze(args.root)
    with args.output.open('x') as file:
        json.dump(result, file, indent=2)
    print(json.dumps({'modes': result['modes'], 'invalid': result['invalid']}, indent=2))
