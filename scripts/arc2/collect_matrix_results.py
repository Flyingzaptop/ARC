"""Extract version-separated matrix medians without inflating large IR dumps."""
import argparse
import json
from pathlib import Path
import zipfile


def collect(paths):
    results = []
    for path in paths:
        with zipfile.ZipFile(path) as archive:
            summary = json.loads(archive.read('summary.json'))
            matrix = json.loads(archive.read('matrix.json'))
            runs = [json.loads(archive.read(item['run_json'].replace('\\', '/')))
                    for item in matrix['runs']]
        hashes = sorted({r['frontend_sha256'] for r in runs if r.get('frontend_sha256')})
        results.append(dict(archive=path.name, workload=summary['workload'],
                            frontend_hashes=hashes,
                            executable_hashes=sorted({r['executable_sha256'] for r in runs}),
                            environment=runs[0].get('environment', {}),
                            measured_runs=sum(r['status'] == 'measured' for r in summary['runs']),
                            total_runs=len(summary['runs']),
                            mode_cadence_median_hz=summary['mode_cadence_median_hz'],
                            native_bracket_adjusted_median_percent=summary.get('native_bracket_adjusted_median_percent'),
                            measurement=summary['measurement']))
    return dict(schema='arc2-matrix-collection-v1', workloads=results,
                warning='Version-separated descriptive results; quality, API coverage and CPU/GPU timing gates require separate evidence. No inferred display FPS.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('archives', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(collect(args.archives), indent=2) + '\n', encoding='utf-8')
