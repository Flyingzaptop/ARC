"""Static gate for a future *isolated* original-code CPU replay.

No executable image is mapped, patched, or called here. The gate reports the
observed address coverage and the exact branch/call sites a separate native
child process would have to trap or support.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_CALL, CS_GRP_JUMP
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

SCRIPTS = Path(__file__).resolve().parents[2] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
from cpu_contract_events import event_records
from cpu_producer_plan import GPRS


def _path(root, value):
    p = Path(value)
    return p if p.is_absolute() else root / p


def _context(root, manifest, stem):
    if stem + '_context_hex' in manifest:
        return bytes.fromhex(manifest[stem + '_context_hex'])
    return _path(root, manifest[stem + '_context_file']).read_bytes()


def _pe_contiguous_function_end(path, entry_rva, last_observed_rva):
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b'MZ':
        raise ValueError('module_image_not_PE')
    nt = struct.unpack_from('<I', data, 0x3c)[0]
    if data[nt:nt + 4] != b'PE\0\0':
        raise ValueError('module_image_not_PE')
    count = struct.unpack_from('<H', data, nt + 6)[0]
    optional_bytes = struct.unpack_from('<H', data, nt + 20)[0]
    first_section = nt + 24 + optional_bytes
    entries = []
    for i in range(count):
        off = first_section + i * 40
        if data[off:off + 8].rstrip(b'\0') != b'.pdata':
            continue
        raw_size, raw_offset = struct.unpack_from('<II', data, off + 16)
        for at in range(raw_offset, raw_offset + raw_size - 11, 12):
            begin, end, _ = struct.unpack_from('<III', data, at)
            if begin < end:
                entries.append((begin, end))
    first = next(((a, b) for a, b in entries if a <= entry_rva < b), None)
    last = next(((a, b) for a, b in entries
                 if a <= last_observed_rva < b), None)
    if first is None or last is None:
        raise ValueError('callee_not_in_PE_pdata')
    cursor = first[1]
    while cursor < last[1]:
        following = next(((a, b) for a, b in entries if a == cursor), None)
        if following is None:
            raise ValueError('callee_pdata_gap')
        cursor = following[1]
    return cursor


def normalize_manifest(manifest, root):
    manifest = dict(manifest)
    if 'module_pe' not in manifest and 'module_image' in manifest:
        manifest['module_pe'] = manifest['module_image']
    if 'append_entry_rva' not in manifest or 'append_end_rva' not in manifest:
        contract = json.loads(_path(root, manifest['append_contract']).read_text())
        manifest.setdefault('append_entry_rva', contract['entry_rva'])
        observed = max(int(x) for x in contract['verified_instruction_bytes'])
        manifest.setdefault('append_end_rva', _pe_contiguous_function_end(
            _path(root, manifest['module_pe']),
            manifest['append_entry_rva'], observed))
    return manifest


def _inside(regions, address, size):
    return any(r['base'] <= address and address + size <= r['base'] + r['size']
               for r in regions)


def audit_manifest(manifest_file):
    manifest_file = Path(manifest_file)
    try:
        manifest = normalize_manifest(json.loads(manifest_file.read_text()),
                                      manifest_file.parent)
    except (KeyError, ValueError, OSError) as exc:
        return {'ready_for_native_replay': False, 'static_preflight_pass': False,
                'errors': ['manifest_normalization_failed:' + str(exc)]}
    root = manifest_file.parent
    required = ('capture', 'regions', 'module_pe', 'module_base',
                'loop_entry_rva',
                'loop_end_rva', 'append_entry_rva', 'append_end_rva',
                'teb_tls_pointer_value', 'append_contract')
    missing = [key for key in required if key not in manifest]
    if missing:
        return {'ready_for_native_replay': False, 'errors': ['missing_manifest_fields:' +
                                                            ','.join(missing)]}
    for stem in ('entry', 'exit'):
        if stem + '_context_hex' not in manifest and stem + '_context_file' not in manifest:
            return {'ready_for_native_replay': False,
                    'errors': ['missing_' + stem + '_context']}
    errors = []
    regions = sorted(manifest['regions'], key=lambda r: r['base'])
    for index, region in enumerate(regions):
        if not {'base', 'size', 'before_file', 'after_file', 'type', 'protect'} <= region.keys():
            errors.append('region_metadata_incomplete')
            continue
        if (type(region['base']) is not int or type(region['size']) is not int or
                region['base'] < 0 or region['size'] <= 0 or
                region['base'] + region['size'] >= 1 << 64):
            errors.append('invalid_region_span')
            continue
        if index and regions[index - 1]['base'] + regions[index - 1]['size'] > region['base']:
            errors.append('overlapping_regions')
        for key in ('before_file', 'after_file'):
            file = _path(root, region[key])
            if not file.is_file() or file.stat().st_size != region['size']:
                errors.append('missing_or_wrong_size_region_file:' + str(file))
    for key in ('module_pe',):
        file = _path(root, manifest[key])
        if not file.is_file() or file.stat().st_size == 0:
            errors.append('missing_or_empty_file:' + key)
    contract_file = _path(root, manifest['append_contract'])
    if not contract_file.is_file():
        errors.append('missing_append_contract')
        contract = None
    else:
        contract = json.loads(contract_file.read_text())
        if contract.get('entry_rva') != manifest['append_entry_rva']:
            errors.append('append_contract_entry_mismatch')
        if contract.get('replacement_admitted') is not False:
            errors.append('append_contract_does_not_disclaim_live_replacement')

    capture = _path(root, manifest['capture'])
    if not (capture / 'capture.json').is_file() or not (capture / 'memory-plan.json').is_file():
        return {'ready_for_native_replay': False,
                'errors': errors + ['missing_capture_metadata']}
    meta = json.loads((capture / 'capture.json').read_text())
    plan = json.loads((capture / 'memory-plan.json').read_text())
    try:
        entry_context = _context(root, manifest, 'entry')
        exit_context = _context(root, manifest, 'exit')
    except (TypeError, ValueError, OSError):
        entry_context = exit_context = b''
    if (len(entry_context) != meta['context_bytes'] or
            len(exit_context) != meta['context_bytes']):
        errors.append('invalid_context_bytes')
    if not meta.get('iteration_mode') or not meta.get('memory_capture_enabled'):
        errors.append('capture_not_whole_iteration_memory_trace')
    if meta['main_base'] != manifest['module_base']:
        errors.append('module_base_mismatch')
    if meta['entry'] - meta['main_base'] != manifest['loop_entry_rva']:
        errors.append('loop_entry_mismatch')
    if meta['entry_end'] - meta['main_base'] != manifest['loop_end_rva']:
        errors.append('loop_end_mismatch')
    events = list(event_records(capture, meta))
    if len(events) != meta['written_events']:
        errors.append('event_count_mismatch')
    if sum(e['kind'] == 1 for e in events) != sum(e['kind'] == 2 for e in events):
        errors.append('incomplete_iteration')
    if events and events[0].get('raw_context_hex') != entry_context.hex():
        errors.append('entry_context_not_from_capture')
    if len(exit_context) == meta['context_bytes']:
        exit_rip = int.from_bytes(exit_context[meta['rip_offset']:
                                               meta['rip_offset'] + 8], 'little')
        if exit_rip - meta['main_base'] != manifest['loop_end_rva']:
            errors.append('exit_context_not_at_natural_loop_end')
    uncovered = []
    failed = []
    for event in events:
        if not event.get('memory_known', False):
            failed.append(event['index'])
        for item in list(event.get('memory', ())) + list(event.get('previous_after', ())):
            patched_gs = (event['code'].startswith('65488b042558000000') and
                          item['address'] == event['teb'] + 0x58 and
                          item['size'] == 8)
            if not item['ok']:
                failed.append(event['index'])
            if not patched_gs and not _inside(regions, item['address'], item['size']):
                uncovered.append((item['address'], item['size']))
    if failed:
        errors.append('memory_operand_unavailable')
    if uncovered:
        errors.append('snapshot_does_not_cover_observed_memory')

    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    gs_sites = []
    direct_calls = []
    indirect_control = []
    branch_targets_outside = []
    instructions = {}
    loop_lo, loop_hi = manifest['loop_entry_rva'], manifest['loop_end_rva']
    append_lo = manifest['append_entry_rva']
    append_hi = manifest['append_end_rva']
    for record in plan['records']:
        rva = record['rva']
        if not (loop_lo <= rva < loop_hi or append_lo <= rva < append_hi):
            continue
        decoded = list(decoder.disasm(bytes.fromhex(record['code']), rva))
        if len(decoded) != 1 or decoded[0].size != record['length']:
            errors.append('instruction_decode_gap')
            continue
        ins = decoded[0]
        instructions[rva] = ins
        for operand in ins.operands:
            if (operand.type == X86_OP_MEM and operand.mem.segment and
                    ins.reg_name(operand.mem.segment) == 'gs'):
                if not (ins.mnemonic == 'mov' and ins.op_str ==
                        'rax, qword ptr gs:[0x58]' and record['length'] == 9):
                    errors.append('unsupported_segment_instruction')
                gs_sites.append(rva)
        if ins.group(CS_GRP_CALL):
            if ins.operands[0].type != X86_OP_IMM:
                indirect_control.append(rva)
            else:
                direct_calls.append((rva, ins.operands[0].imm))
        if ins.group(CS_GRP_JUMP):
            if ins.operands[0].type != X86_OP_IMM:
                indirect_control.append(rva)
            else:
                target = ins.operands[0].imm
                if not (loop_lo <= target < loop_hi or append_lo <= target < append_hi):
                    branch_targets_outside.append((rva, target))
    if indirect_control:
        errors.append('indirect_control_flow')
    if branch_targets_outside:
        errors.append('branch_target_outside_copied_spans')
    other_calls = [(rva, target) for rva, target in direct_calls
                   if target != append_lo]
    # All other calls require explicit trap sites. A capacity guard alone does
    # not close the TLS initializer or allocation paths.
    required_traps = sorted({rva for rva, _ in other_calls})
    if ('trap_call_rvas' in manifest and
            not set(required_traps) <= set(manifest['trap_call_rvas'])):
        errors.append('untrapped_out_of_span_call')
    if len(gs_sites) != 2:
        errors.append('unexpected_gs_load_sites')
    loop_bound = None
    backedges = [
        (rva, ins) for rva, ins in instructions.items()
        if loop_lo <= rva < loop_hi and ins.mnemonic == 'jne' and
        ins.operands[0].type == X86_OP_IMM and
        ins.operands[0].imm == loop_lo
    ]
    if len(backedges) == 1:
        branch_rva, _ = backedges[0]
        preceding = [(rva, instructions[rva]) for rva in sorted(instructions)
                     if loop_lo <= rva < branch_rva]
        if len(preceding) >= 2:
            cmp_rva, compare = preceding[-1]
            add_rva, advance = preceding[-2]
            if (compare.mnemonic == 'cmp' and advance.mnemonic == 'add' and
                    len(compare.operands) == len(advance.operands) == 2 and
                    compare.operands[0].type == X86_OP_REG and
                    compare.operands[1].type == X86_OP_MEM and
                    advance.operands[0].type == X86_OP_REG and
                    advance.operands[1].type == X86_OP_IMM):
                register = compare.reg_name(compare.operands[0].reg)
                bound = compare.operands[1].mem
                if (register == advance.reg_name(advance.operands[0].reg) and
                        compare.reg_name(bound.base) == 'rsp' and
                        not bound.index and bound.disp >= 0 and
                        advance.operands[1].imm > 0 and
                        compare.operands[1].size == 8):
                    loop_bound = {
                        'advance_rva': add_rva, 'compare_rva': cmp_rva,
                        'backedge_rva': branch_rva, 'register': register,
                        'stack_displacement': bound.disp,
                        'step': advance.operands[1].imm,
                    }
    if loop_bound is None:
        errors.append('unsupported_loop_bound_shape')
    full_count = None
    if loop_bound and loop_bound['register'] in GPRS and entry_context and exit_context:
        register_offset = meta['rax_offset'] + 8 * GPRS.index(loop_bound['register'])
        first = int.from_bytes(entry_context[register_offset:register_offset + 8],
                               'little')
        last = int.from_bytes(exit_context[register_offset:register_offset + 8],
                              'little')
        difference = last - first
        if difference <= 0 or difference % loop_bound['step']:
            errors.append('natural_loop_count_not_integral')
        else:
            full_count = difference // loop_bound['step']
            if full_count < sum(e['kind'] == 1 for e in events):
                errors.append('exit_precedes_traced_prefix')
    else:
        errors.append('unsupported_context_loop_register')
    return {
        'ready_for_native_replay': False,
        'static_preflight_pass': not errors,
        'errors': sorted(set(errors)),
        'observed_events': len(events),
        'regions': len(regions),
        'uncovered_observed_memory': len(set(uncovered)),
        'failed_memory_events': len(set(failed)),
        'gs_load_rvas': sorted(set(gs_sites)),
        'direct_calls': [{'rva': rva, 'target_rva': target}
                         for rva, target in direct_calls],
        'required_trap_call_rvas': required_traps,
        'indirect_control_rvas': sorted(set(indirect_control)),
        'branch_targets_outside': branch_targets_outside,
        'loop_bound': loop_bound,
        'natural_iteration_count': full_count,
        'scope': 'static observed-address and code-class gate only; never live replacement',
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('manifest', type=Path)
    args = parser.parse_args()
    result = audit_manifest(args.manifest)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result.get('static_preflight_pass') else 2)
