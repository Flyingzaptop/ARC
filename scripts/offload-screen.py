"""O0 diagnostic screen, never a production selector or an offload admission."""
import argparse
import csv
import hashlib
import json
import statistics
import struct
import uuid
from collections import defaultdict
from pathlib import Path


def pe_identity(path):
    data = path.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[:2] != b'MZ' or data[pe:pe+4] != b'PE\0\0':
        raise ValueError('Not a PE image')
    machine, sections, stamp = struct.unpack_from('<HHI', data, pe+4)
    optional_size = struct.unpack_from('<H', data, pe+20)[0]
    optional = pe+24
    if machine != 0x8664 or struct.unpack_from('<H', data, optional)[0] != 0x20b:
        raise ValueError('Expected x64 PE32+')
    section = optional+optional_size
    mappings = [struct.unpack_from('<IIII', data, section+40*i+8) for i in range(sections)]
    def file_offset(rva):
        for virtual_size, address, raw_size, raw in mappings:
            if address <= rva < address+min(virtual_size, raw_size):
                return raw+rva-address
        raise ValueError('RVA not backed by file')
    debug_rva, debug_size = struct.unpack_from('<II', data, optional+112+6*8)
    debug = file_offset(debug_rva)
    for offset in range(debug, debug+debug_size, 28):
        _, _, _, _, kind, size, _, raw = struct.unpack_from('<IIHHIIII', data, offset)
        if kind == 2 and data[raw:raw+4] == b'RSDS' and size >= 24:
            return {'sha256': hashlib.sha256(data).hexdigest(), 'pe_timestamp': stamp,
                    'pdb_guid': str(uuid.UUID(bytes_le=data[raw+4:raw+20])),
                    'pdb_age': struct.unpack_from('<I', data, raw+20)[0],
                    'pdb_path_recorded': data[raw+24:raw+size].split(b'\0')[0].decode('utf-8')}
    raise ValueError('No RSDS identity')


def stats(values):
    values = sorted(values)
    if not values:
        return None
    return {'count': len(values), 'mean_ms': statistics.mean(values),
            'p50_ms': statistics.median(values),
            'p95_ms': values[int((len(values)-1)*.95)],
            'p99_ms': values[int((len(values)-1)*.99)]}


def screen(path, warmup_ms=6000):
    groups = defaultdict(dict)
    duplicates = 0
    with path.open(encoding='utf-8-sig', newline='') as stream:
        for row in csv.DictReader(stream):
            if float(row['elapsed_ms']) < warmup_ms or int(row['frame']) <= 10:
                continue
            key = int(row['frame'])
            if key in groups[row['event']]:
                duplicates += 1
                continue
            groups[row['event']][key] = (float(row['elapsed_ms']), float(row['ms']))
    submit = groups['SubmitCommandLists']
    times = [submit[k][0]-submit[k-1][0] for k in sorted(submit) if k-1 in submit]
    periods = stats(times)
    floor = max(.5, .05*periods['mean_ms']) if periods else None
    residual = [groups['Application Update'][f][1]-v[1]
                for f, v in groups['RenderPath3D Update'].items()
                if f in groups['Application Update']]
    candidates = [
        ('render_command_construction', 'Application Render'),
        ('visibility', 'Frustum Culling'),
        ('instance_update_residual', None),
        ('submission', 'SubmitCommandLists'),
        ('buffer_update', 'Update Buffers (CPU)'),
    ]
    ranked = []
    for candidate, label in candidates:
        values = residual if label is None else [v[1] for v in groups[label].values()]
        measurement = stats(values)
        ranked.append({'candidate': candidate, 'timer': label, 'wall': measurement,
                       'running_ms': None, 'waiting_ms': None,
                       'critical_path_saving_upper_bound_ms': measurement['mean_ms'] if measurement else None,
                       'critical_path_saving_lower_bound_ms': 0,
                       'scope': 'aggregate elapsed upper bound, not isolated CPU kernel time',
                       'exceeds_screen_floor': measurement['mean_ms'] >= floor if measurement and floor else None})
    ranked.sort(key=lambda item: item['wall']['mean_ms'] if item['wall'] else 0, reverse=True)
    return {'diagnostic_only': True, 'runtime_admission': False, 'warmup_ms': warmup_ms,
            'cpu_submit_cadence': periods, 'cadence_scope': 'CPU submission completion timestamps, not GPU/display completion',
            'screen_gain_floor_ms': floor, 'control_drift_ms': None, 'final_O2_gate': 'not_evaluated',
            'duplicate_event_frame_rows': duplicates, 'ranked_candidates': ranked,
            'profiler_alignment': 'completed profiler ranges read before HarnessUpdate increments frame ID; no manual frame shift',
            'residual_scope': 'Application Update minus nested RenderPath3D Update; an upper bound including unisolated work'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--csv', required=True, type=Path)
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    result = screen(args.csv)
    result['application'] = pe_identity(args.exe)
    result['csv_sha256'] = hashlib.sha256(args.csv.read_bytes()).hexdigest()
    args.out.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(args.out)


if __name__ == '__main__':
    main()
