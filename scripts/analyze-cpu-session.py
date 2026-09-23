"""Summarize one CPU session, preserving missing evidence and cost scope."""
import argparse
import json
from pathlib import Path


def analyze(folder):
    session = json.loads((folder / 'session.json').read_text(encoding='utf-8-sig'))
    report_path = folder / 'cpu-runtime.json'
    result = {
        'schema': 1, 'mode': session.get('mode'), 'session_status': session.get('status'),
        'target_sha256': session.get('target_sha256'),
        'client_sha256': session.get('client_sha256'),
        'game_fps_benefit': 'unknown', 'frame_gpu_correlation': 'not_collected_by_cpu_launcher',
        'missing': [],
    }
    if not report_path.exists():
        result['runtime_evidence'] = 'not_expected' if result['mode'] == 'baseline' else 'missing'
        if result['mode'] != 'baseline':
            result['missing'].append('cpu-runtime.json; no execution claim is possible')
        return result
    report = json.loads(report_path.read_text(encoding='utf-8-sig'))
    result['runtime_evidence'] = 'present'
    result['identity_consistent'] = (report.get('mode') == result['mode']
        and session.get('target_pid') is not None
        and report.get('pid') == session.get('target_pid'))
    if not result['identity_consistent']:
        result['missing'].append('mode/PID missing or mismatched: report cannot establish this session')
    if not session.get('target_creation_utc'):
        result['missing'].append('target creation time unavailable; no cross-trace lifetime correlation')
    for key in ('backend', 'regions_discovered', 'regions_rejected', 'calls', 'executions',
                'guard_hits', 'guard_misses', 'skipped_ops', 'dirty_nodes', 'code_changed',
                'stopped', 'stop_control_available', 'dx12_conflict', 'profitability', 'study_complete', 'capture'):
        result[key] = report.get(key)
    executions = report.get('executions')
    result['transformed_work_observed'] = (
        isinstance(executions, int) and executions > 0 and result['identity_consistent'])
    result['interpretation'] = (
        'Supported CPU work executed through an actuator; this is not a net speedup measurement.'
        if result['transformed_work_observed'] else
        'No transformed CPU work established for this session.')
    result['raw_runtime'] = report
    result['retained_region_slots'] = len(report.get('regions', []))
    ranked = []
    for region in report.get('regions', []):
        calls = region.get('calls', 0)
        operations = region.get('instructions', 0)
        ranked.append({
            'id': region.get('id'), 'module_offset': region.get('module_offset'),
            'code_hash': region.get('code_hash'), 'calls': calls,
            'work_proxy_instructions': calls * operations,
            'cost_scope': region.get('cost_scope', 'instruction_count_proxy_not_CPU_nanoseconds'),
            'observed_executions': region.get('executions'),
            'supported_opportunities': ['guarded_specialization', 'register_memoization', 'acyclic_incremental'],
            'admission_active': region.get('active'),
            'profitability': {1: 'positive_within_DBI_estimate', 2: 'rejected_within_DBI_cost'}.get(region.get('cost_reason'), 'not_established'),
            'action_median_ns': region.get('action_median_ns'),
            'cost_aggregation': region.get('cost_aggregation'),
            'cost_thread_id': region.get('cost_thread_id'),
            'selected_action': {0: 'original', 1: 'specialize', 2: 'memo', 3: 'incremental'}.get(region.get('cost_action')),
            'auto_trial_executions': region.get('auto_trial_executions'),
            'auto_policy_executions': region.get('auto_policy_executions'),
        })
    ranked.sort(key=lambda r: r['work_proxy_instructions'], reverse=True)
    result['ranked_cpu_regions'] = ranked
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('session', type=Path)
    args = parser.parse_args()
    summary = analyze(args.session)
    output = args.session / 'summary.json'
    output.write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding='utf-8')
    print(output)


if __name__ == '__main__':
    main()
