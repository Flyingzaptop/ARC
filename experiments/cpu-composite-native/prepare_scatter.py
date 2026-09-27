"""Static gate and v3 pack for isolated original word-scatter loop replay."""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_CALL, CS_GRP_JUMP
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'scripts'))
from cpu_contract_events import event_records
from cpu_producer_plan import GPRS
from preflight import _context, _inside, _path
from prepare import _text, _u32, _u64


def _register(raw, meta, name):
    return struct.unpack_from(
        '<Q', raw, meta['rax_offset'] + 8 * GPRS.index(name))[0]


def _view_bytes(regions, root, address, size, *, after=False):
    for region in regions:
        if region['base'] <= address and address + size <= region['base'] + region['size']:
            path = _path(root, region['after_file' if after else 'before_file'])
            with path.open('rb') as f:
                f.seek(address - region['base'])
                value = f.read(size)
            if len(value) == size:
                return value
    raise ValueError('address_not_in_captured_view')


def _ins(decoder, record):
    code = bytes.fromhex(record['code'])
    instructions = list(decoder.disasm(code, record['rva']))
    if len(instructions) != 1 or instructions[0].size != len(code):
        raise ValueError('instruction_decode_gap')
    return instructions[0]


def prepare_scatter_pack(manifest_file, output):
    manifest_file = Path(manifest_file)
    root = manifest_file.parent
    manifest = json.loads(manifest_file.read_text())
    required = ('capture', 'regions', 'module_image', 'module_base',
                'loop_entry_rva', 'loop_end_rva', 'word_contract')
    if any(key not in manifest for key in required):
        raise ValueError('scatter_manifest_fields_missing')
    capture = _path(root, manifest['capture'])
    meta = json.loads((capture / 'capture.json').read_text())
    plan = json.loads((capture / 'memory-plan.json').read_text())
    records = {r['rva']: r for r in plan['records']}
    contract = json.loads(_path(root, manifest['word_contract']).read_text())
    if (not meta.get('iteration_mode') or not meta.get('memory_capture_enabled') or
            meta['main_base'] != manifest['module_base'] or
            meta['entry'] - meta['main_base'] != manifest['loop_entry_rva'] or
            meta['entry_end'] - meta['main_base'] != manifest['loop_end_rva']):
        raise ValueError('capture_or_loop_boundary_mismatch')
    events = list(event_records(capture, meta))
    if (len(events) != meta['written_events'] or
            sum(e['kind'] == 1 for e in events) !=
            sum(e['kind'] == 2 for e in events)):
        raise ValueError('incomplete_iteration_prefix')
    entry = _context(root, manifest, 'entry')
    exit_state = _context(root, manifest, 'exit')
    if len(entry) != meta['context_bytes'] or len(exit_state) != meta['context_bytes']:
        raise ValueError('context_size_invalid')
    if (events[0]['raw_context_hex'] != entry.hex() or
            struct.unpack_from('<Q', exit_state, meta['rip_offset'])[0] !=
            meta['main_base'] + manifest['loop_end_rva']):
        raise ValueError('entry_or_natural_exit_context_mismatch')
    regions = manifest['regions']
    for region in regions:
        if not {'base', 'size', 'before_file', 'after_file', 'protect'} <= region.keys():
            raise ValueError('region_metadata_incomplete')
        for key in ('before_file', 'after_file'):
            if _path(root, region[key]).stat().st_size != region['size']:
                raise ValueError('view_file_size_mismatch')
    for event in events:
        if not event.get('memory_known', False):
            raise ValueError('unclosed_memory_operand')
        for item in list(event.get('memory', ())) + list(event.get('previous_after', ())):
            if not item['ok'] or not _inside(regions, item['address'], item['size']):
                raise ValueError('observed_memory_outside_views')

    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    lo, hi = manifest['loop_entry_rva'], manifest['loop_end_rva']
    instructions = {rva: _ins(decoder, record)
                    for rva, record in records.items() if lo <= rva < hi}
    tail = contract['capture_hints']['tail']
    if len(tail) != 3 or any(
            x['rva'] not in instructions or
            records[x['rva']]['code'] != x['code'] for x in tail):
        raise ValueError('tail_machine_bytes_changed')
    advance, compare, branch = (instructions[x['rva']] for x in tail)
    cursor_reg = contract['capture_hints']['input_cursor_register']
    end_reg = contract['capture_hints']['input_end_register']
    if (advance.mnemonic != 'add' or
            advance.reg_name(advance.operands[0].reg) != cursor_reg or
            advance.operands[1].type != X86_OP_IMM or
            compare.mnemonic != 'cmp' or
            compare.reg_name(compare.operands[0].reg) != cursor_reg or
            compare.reg_name(compare.operands[1].reg) != end_reg or
            branch.mnemonic != 'jne' or
            branch.operands[0].imm != lo):
        raise ValueError('register_bound_loop_shape_changed')
    input_step = advance.operands[1].imm
    first_cursor = _register(entry, meta, cursor_reg)
    last_cursor = _register(exit_state, meta, cursor_reg)
    if (input_step <= 0 or last_cursor <= first_cursor or
            (last_cursor - first_cursor) % input_step or
            _register(entry, meta, end_reg) != last_cursor):
        raise ValueError('natural_iteration_count_invalid')
    iterations = (last_cursor - first_cursor) // input_step
    if iterations < sum(e['kind'] == 1 for e in events):
        raise ValueError('exit_precedes_sampled_prefix')

    word = contract['word_scatter']
    store_rva = word['store_rva']
    if store_rva not in instructions or records[store_rva]['code'] != word['store_code']:
        raise ValueError('scatter_store_machine_bytes_changed')
    store = instructions[store_rva]
    recipe = word['address_recipe']
    if (store.mnemonic != 'mov' or store.operands[0].type != X86_OP_MEM or
            store.operands[0].size != 4 or recipe['scale'] != 4 or
            store.reg_name(store.operands[0].mem.base) != recipe['base'] or
            store.reg_name(store.operands[0].mem.index) != recipe['index'] or
            store.operands[0].mem.disp != recipe['disp']):
        raise ValueError('scatter_store_shape_changed')
    index_reg = word['index_register']
    first_index = _register(entry, meta, index_reg)
    last_index = _register(exit_state, meta, index_reg)
    if last_index <= first_index or last_index - first_index > iterations * 16:
        raise ValueError('scatter_count_invalid')
    pointer_recipe = contract['capture_hints']['output_base_pointer_address_recipe']
    if pointer_recipe['index'] is not None or pointer_recipe['segment'] is not None:
        raise ValueError('unsupported_output_base_pointer_recipe')
    pointer_address = (_register(entry, meta, pointer_recipe['base']) +
                       pointer_recipe['disp'])
    if pointer_address != contract['capture_hints']['output_base_pointer_read_address']:
        raise ValueError('output_pointer_read_address_changed')
    output_base = int.from_bytes(
        _view_bytes(regions, root, pointer_address, 8), 'little')
    if output_base != contract['capture_hints']['output_base_observed']:
        raise ValueError('output_pointer_value_changed')
    output_start = output_base + first_index * 4
    output_end = output_base + last_index * 4
    output_region = next((r for r in regions
                          if r['base'] <= output_start < output_end <=
                          r['base'] + r['size']), None)
    if output_region is None or output_region['protect'] != 0x404:
        raise ValueError('writecombine_output_span_not_captured')
    stores = [e for e in events if e['rip'] - meta['main_base'] == store_rva]
    if len(stores) != sum(e['kind'] == 1 for e in events):
        raise ValueError('sampled_store_count_not_one_per_iteration')
    for i, event in enumerate(stores):
        address = output_start + i * 4
        if event['memory'][0]['address'] != address or not event['memory'][0]['ok']:
            raise ValueError('sampled_scatter_address_not_contiguous')
        after_event = events[event['index'] + 1]
        if not any(x['address'] == address and x['size'] == 4 and x['ok']
                   for x in after_event.get('previous_after', ())):
            raise ValueError('sampled_scatter_postimage_missing')

    calls = []
    gs_sites = []
    for rva, ins in instructions.items():
        if ins.group(CS_GRP_CALL):
            calls.append(rva)  # direct and indirect both trap before execution
        if ins.group(CS_GRP_JUMP):
            if ins.operands[0].type != X86_OP_IMM or not lo <= ins.operands[0].imm <= hi:
                raise ValueError('unsupported_jump_outside_loop')
        if any(o.type == X86_OP_MEM and o.mem.segment and
               ins.reg_name(o.mem.segment) == 'gs' for o in ins.operands):
            gs_sites.append(rva)
    if gs_sites:
        raise ValueError('scatter_GS_instruction_unexpected')
    # For warm repeats, every increment of the output index must be followed
    # by this store without intervening control flow. The full output span is
    # then overwritten regardless of the per-iteration group size.
    index_writers = [(rva, ins) for rva, ins in instructions.items()
                     if ins.operands and ins.operands[0].type == X86_OP_REG and
                     ins.reg_name(ins.operands[0].reg) in (index_reg, 'e' + index_reg[1:])]
    inc_only = (len(index_writers) == 1 and
                index_writers[0][1].mnemonic == 'inc' and
                index_writers[0][0] < store_rva and
                not any(index_writers[0][0] < rva < store_rva and
                        (ins.group(CS_GRP_JUMP) or ins.group(CS_GRP_CALL))
                        for rva, ins in instructions.items()))
    stack_ranges = sorted({(r['address'], r['address'] + r['width'])
                           for r in contract['stack_write_recipes']})
    stack_pointer = _register(entry, meta, 'rsp')
    stack_region = next((r for r in regions if
                         r['base'] <= stack_pointer < r['base'] + r['size']), None)
    if stack_region is None or any(
            not (stack_region['base'] <= a < b <=
                 stack_region['base'] + stack_region['size'])
            for a, b in stack_ranges):
        raise ValueError('declared_stack_write_outside_stack_view')

    observed_rvas = sorted({e['rip'] - meta['main_base'] for e in events
                            if e['rip'] - meta['main_base'] in records})
    with Path(output).open('wb') as f:
        f.write(b'ARCRPL01')
        _u32(f, 3)
        _u64(f, manifest['module_base'])
        for value in (lo, hi, 0, 0):
            _u32(f, value)
        _u64(f, 0)  # no GS/TLS value
        _u64(f, 0)  # no append container
        for value in (0, 0, 4, iterations):
            _u32(f, value)
        _u64(f, output_end)
        _u32(f, int(inc_only))
        _text(f, _path(root, manifest['module_image']).resolve())
        _u32(f, len(entry)); f.write(entry)
        _u32(f, len(exit_state)); f.write(exit_state)
        _u32(f, 0)  # GS sites
        _u32(f, len(calls))
        for rva in sorted(calls): _u32(f, rva)
        _u32(f, len(regions))
        for region in regions:
            _u64(f, region['base']); _u64(f, region['size'])
            _text(f, _path(root, region['before_file']).resolve())
            _text(f, _path(root, region['after_file']).resolve())
            _u32(f, region['protect'])
        _u32(f, len(observed_rvas))
        for rva in observed_rvas:
            code = bytes.fromhex(records[rva]['code'])
            _u32(f, rva); _u32(f, len(code)); f.write(code)
        for value in (pointer_address, output_base, output_start, output_end):
            _u64(f, value)
        _u32(f, len(stack_ranges))
        for a, b in stack_ranges:
            _u64(f, a); _u64(f, b)
    return {
        'pack': str(Path(output).resolve()),
        'version': 3,
        'natural_iterations': iterations,
        'output_words': last_index - first_index,
        'output_start': output_start,
        'output_end': output_end,
        'output_protection': output_region['protect'],
        'stack_write_ranges': len(stack_ranges),
        'trapped_call_rvas': sorted(calls),
        'full_output_overwrite_proven': inc_only,
        'native_execution_started': False,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('manifest', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare_scatter_pack(args.manifest, args.output), indent=2))
