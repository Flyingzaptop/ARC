"""Check whether only resettable private bytes changed in captured views."""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from preflight import _context, _path, normalize_manifest


def analyze_warm_reset(manifest_file):
    manifest_file = Path(manifest_file)
    root = manifest_file.parent
    manifest = normalize_manifest(json.loads(manifest_file.read_text()), root)
    contract = json.loads(_path(root, manifest['append_contract']).read_text())
    stride = contract['record_stride']
    first_write = contract['ordered_writes'][0]
    full_record_zero = (first_write.get('offset') == 0 and
                        first_write.get('width') == stride and
                        first_write.get('operation') in ('vmovups', 'vmovdqu'))
    entry = _context(root, manifest, 'entry')
    capture_meta = json.loads((_path(root, manifest['capture']) /
                               'capture.json').read_text())
    stack_pointer = struct.unpack_from(
        '<Q', entry, capture_meta['rax_offset'] + 4 * 8)[0]
    regions = manifest['regions']
    stacks = [r for r in regions
              if r['base'] <= stack_pointer < r['base'] + r['size']]
    if len(stacks) != 1:
        raise ValueError('entry_stack_region_not_unique')
    stack = stacks[0]
    cursor_address = manifest.get('cursor_address')
    initial_cursor = manifest.get('initial_cursor')
    final_cursor = manifest.get('expected_final_cursor')
    if any(type(x) is not int for x in
           (cursor_address, initial_cursor, final_cursor)):
        raise ValueError('manifest_missing_cursor_or_output_range')
    if not initial_cursor <= final_cursor:
        raise ValueError('invalid_output_range')
    if ((final_cursor - initial_cursor) % stride or
            (final_cursor - initial_cursor) // stride >
            manifest.get('packet_count', 0)):
        raise ValueError('output_count_invalid')
    allowed = [(stack['base'], stack['base'] + stack['size']),
               (cursor_address, cursor_address + 8),
               (initial_cursor, final_cursor)]
    changed_allowed = changed_other = 0
    outside_examples = []
    per_region = []
    for region in regions:
        before = _path(root, region['before_file'])
        after = _path(root, region['after_file'])
        if before.stat().st_size != region['size'] or after.stat().st_size != region['size']:
            raise ValueError('view_file_size_mismatch')
        region_allowed = region_other = 0
        with before.open('rb') as a, after.open('rb') as b:
            position = 0
            while position < region['size']:
                n = min(1 << 20, region['size'] - position)
                x, y = a.read(n), b.read(n)
                if len(x) != n or len(y) != n:
                    raise ValueError('truncated_view')
                if x != y:
                    for i, (left, right) in enumerate(zip(x, y)):
                        if left == right:
                            continue
                        address = region['base'] + position + i
                        if any(lo <= address < hi for lo, hi in allowed):
                            region_allowed += 1
                        else:
                            region_other += 1
                            if len(outside_examples) < 12:
                                outside_examples.append(address)
                position += n
        changed_allowed += region_allowed
        changed_other += region_other
        per_region.append({'base': region['base'],
                           'changed_allowed_bytes': region_allowed,
                           'changed_outside_allowed_bytes': region_other})
    return {
        'warm_reset_admitted': full_record_zero and changed_other == 0,
        'full_record_zero_observed': full_record_zero,
        'stack_region': {'base': stack['base'], 'size': stack['size']},
        'cursor_address': cursor_address,
        'output_start': initial_cursor,
        'output_end': final_cursor,
        'changed_allowed_bytes': changed_allowed,
        'changed_outside_allowed_bytes': changed_other,
        'outside_examples_hex': [hex(a) for a in outside_examples],
        'per_region': per_region,
        'scope': 'captured before/after byte differences only; ownership remains open',
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('manifest', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze_warm_reset(args.manifest), indent=2))
