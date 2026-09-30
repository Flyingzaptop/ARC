"""Diagnostic-only interceptor wall-cost, excluding warmup by parent QPC.

This sums instrumented wall time across threads, not critical-path CPU time.
The instrumented DLL is intentionally separate from performance matrix builds.
"""
import argparse
import json
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    index = fraction * (len(ordered) - 1)
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def summarize(meter, run):
    columns = meter['frame_columns']
    if 'qpc' not in columns:
        raise ValueError('meter predates QPC window alignment')
    start, end = run['measurement_start_qpc'], run['measurement_end_qpc']
    frames = [dict(zip(columns, row)) for row in meter['frames']]
    frames = [f for f in frames if start <= f['qpc'] <= end]
    swapchains = {f['swapchain'] for f in frames}
    if len(swapchains) != 1:
        raise ValueError('requires one measured swapchain; totals are process-wide')
    if len(frames) < 2:
        raise ValueError('insufficient measured frames')
    first, last = frames[0], frames[-1]
    intervals = len(frames) - 1
    totals = {key: last[key] - first[key]
              for key in ('own_ns', 'excluded_native_ns', 'calls')}
    if any(value < 0 for value in totals.values()):
        raise ValueError('nonmonotonic meter totals')
    own_intervals = [(b['own_ns'] - a['own_ns']) / 1e6
                     for a, b in zip(frames, frames[1:])]
    if any(value < 0 for value in own_intervals):
        raise ValueError('nonmonotonic meter interval')
    sites = sorted(meter['sites'], key=lambda row: row[1], reverse=True)
    return {
        'schema': 'arc2-cpu-diagnostic-summary-v1',
        'interpretation': meter['interpretation'],
        'frontend_sha256': run['frontend_sha256'],
        'measured_present_intervals': intervals,
        'measured_seconds': (last['qpc'] - first['qpc']) / run['qpc_frequency'],
        'aggregate_interceptor_wall_ms_per_present': totals['own_ns'] / intervals / 1e6,
        'aggregate_interceptor_interval_ms': {
            f'p{int(p * 100)}': percentile(own_intervals, p) for p in (0.5, 0.95, 0.99)},
        'excluded_native_envelope_ms_per_present': totals['excluded_native_ns'] / intervals / 1e6,
        'instrumented_calls_per_present': totals['calls'] / intervals,
        'site_totals_scope': 'whole session including warmup, not measurement-window attribution',
        'top_session_sites': sites[:20],
        'dropped_frames': meter['dropped_frames'],
        'site_overflow': meter['site_overflow'],
        'limits': ['Includes metering disturbance.',
                   'Parallel threads contribute summed wall time.',
                   'Background analysis is outside this boundary meter.',
                   'Not native CPUft, display FPS or proof of performance acceptance.'],
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('meter', type=Path)
    parser.add_argument('run', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    data = summarize(json.loads(args.meter.read_text(encoding='utf-8-sig')),
                     json.loads(args.run.read_text(encoding='utf-8-sig')))
    args.output.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(data, indent=2))
