"""Create a bounded native replay pack after static validation.

This only writes data. The native child is intentionally never launched here.
"""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from preflight import _context, _inside, _path, audit_manifest, normalize_manifest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from cpu_append_contract import preflight_packet_no_growth
from cpu_contract_events import event_records


def _u32(stream, value):
    stream.write(struct.pack('<I', value))


def _u64(stream, value):
    stream.write(struct.pack('<Q', value))


def _text(stream, value):
    raw = str(value).encode('utf-8')
    _u32(stream, len(raw))
    stream.write(raw)


def _read_pointer(regions, address, *, after=False):
    for region in regions:
        if region['base'] <= address and address + 8 <= region['base'] + region['size']:
            with Path(region['after_file' if after else 'before_file']).open('rb') as f:
                f.seek(address - region['base'])
                data = f.read(8)
            if len(data) == 8:
                return int.from_bytes(data, 'little')
    raise ValueError('header_pointer_not_in_before_snapshot')


def prepare_pack(manifest_file, output):
    manifest_file = Path(manifest_file)
    manifest = normalize_manifest(json.loads(manifest_file.read_text()),
                                  manifest_file.parent)
    audit = audit_manifest(manifest_file)
    if not audit.get('static_preflight_pass'):
        raise ValueError('static_preflight_failed:' + ','.join(audit['errors']))
    base = manifest_file.parent
    capture = _path(base, manifest['capture'])
    meta = json.loads((capture / 'capture.json').read_text())
    events = list(event_records(capture, meta))
    plan = {r['rva']: r for r in
            json.loads((capture / 'memory-plan.json').read_text())['records']}
    contract = json.loads(_path(base, manifest['append_contract']).read_text())
    owner_values = {e['registers'][1] for e in events
                    if e['rip'] - meta['main_base'] == contract['entry_rva']}
    if len(owner_values) != 1:
        raise ValueError('append_container_not_unique')
    owner = owner_values.pop()
    regions = [{**r, 'before_file': str(_path(base, r['before_file']).resolve()),
                'after_file': str(_path(base, r['after_file']).resolve())}
               for r in manifest['regions']]
    cursor_offset = contract['container']['cursor_offset']
    capacity_offset = contract['container']['capacity_offset']
    cursor = _read_pointer(regions, owner + cursor_offset)
    capacity = _read_pointer(regions, owner + capacity_offset)
    output_region = next((r for r in regions
                          if r['base'] <= cursor <= capacity <=
                          r['base'] + r['size']), None)
    if output_region is None:
        raise ValueError('append_output_not_contiguous_in_snapshot')
    packet_count = audit['natural_iteration_count']
    loop_bound = audit['loop_bound']
    bound_address = events[0]['registers'][4] + loop_bound['stack_displacement']
    original_bound = _read_pointer(regions, bound_address)
    compares = [e for e in events
                if e['rip'] - meta['main_base'] == loop_bound['compare_rva']]
    if not compares or any(
            e['memory'][0]['address'] != bound_address or
            int.from_bytes(bytes.fromhex(e['memory'][0]['bytes']), 'little') != original_bound
            for e in compares):
        raise ValueError('natural_loop_bound_snapshot_mismatch')
    expected_final_cursor = _read_pointer(
        regions, owner + cursor_offset, after=True)
    if (expected_final_cursor < cursor or
            (expected_final_cursor - cursor) % contract['record_stride'] or
            expected_final_cursor > capacity or
            (expected_final_cursor - cursor) //
            contract['record_stride'] > packet_count):
        raise ValueError('after_snapshot_append_cursor_invalid')
    bounds = preflight_packet_no_growth(
        contract, packet_count=packet_count, begin=cursor, cursor=cursor,
        capacity=capacity,
        snapshot_bounds=(output_region['base'],
                         output_region['base'] + output_region['size']),
        header_range=(owner, owner + max(cursor_offset, capacity_offset) + 8))
    if not bounds['input_guards_pass']:
        raise ValueError('packet_capacity_guard_failed:' + str(bounds['reason']))
    tls = manifest['teb_tls_pointer_value']
    gs_sites = set(audit['gs_load_rvas'])
    gs_values = {
        int.from_bytes(bytes.fromhex(e['memory'][0]['bytes']), 'little')
        for e in events if e['rip'] - meta['main_base'] in gs_sites
    }
    if gs_values != {tls}:
        raise ValueError('captured_gs_pointer_changed')
    for region in regions:
        if not _inside(regions, region['base'], region['size']):
            raise ValueError('invalid_region_range')
    context = _context(base, manifest, 'entry')
    exit_context = _context(base, manifest, 'exit')
    first_record_write = contract['ordered_writes'][0]
    full_record_zero = (
        first_record_write.get('offset') == 0 and
        first_record_write.get('width') == contract['record_stride'] and
        first_record_write.get('operation') in ('vmovups', 'vmovdqu'))
    traps = sorted(set(audit['required_trap_call_rvas'] +
                       [contract['machine_path']['growth_target_rva']]))
    with Path(output).open('wb') as f:
        f.write(b'ARCRPL01')
        _u32(f, 2)
        _u64(f, manifest['module_base'])
        for key in ('loop_entry_rva', 'loop_end_rva', 'append_entry_rva',
                    'append_end_rva'):
            _u32(f, manifest[key])
        _u64(f, tls)
        _u64(f, owner)
        _u32(f, cursor_offset)
        _u32(f, capacity_offset)
        _u32(f, contract['record_stride'])
        _u32(f, packet_count)
        _u64(f, expected_final_cursor)
        _u32(f, int(full_record_zero))
        _text(f, _path(base, manifest['module_pe']).resolve())
        _u32(f, len(context))
        f.write(context)
        _u32(f, len(exit_context))
        f.write(exit_context)
        _u32(f, len(audit['gs_load_rvas']))
        for rva in audit['gs_load_rvas']:
            _u32(f, rva)
        _u32(f, len(traps))
        for rva in traps:
            _u32(f, rva)
        _u32(f, len(regions))
        for r in regions:
            _u64(f, r['base'])
            _u64(f, r['size'])
            _text(f, r['before_file'])
            _text(f, r['after_file'])
        executed = sorted({e['rip'] - meta['main_base'] for e in events
                           if e['rip'] - meta['main_base'] in plan})
        _u32(f, len(executed))
        for rva in executed:
            code = bytes.fromhex(plan[rva]['code'])
            _u32(f, rva)
            _u32(f, len(code))
            f.write(code)
    return {'pack': str(Path(output).resolve()), 'regions': len(regions),
            'packet_count': packet_count, 'max_output_bytes':
            packet_count * contract['record_stride'], 'trap_rvas': traps,
            'expected_append_count': (expected_final_cursor - cursor) //
            contract['record_stride'],
            'native_execution_started': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('manifest', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare_pack(args.manifest, args.output), indent=2))
