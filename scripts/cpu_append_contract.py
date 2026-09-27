"""Byte-checked, sampled no-growth append shape for an *isolated* buffer.

This is a description of one machine-code path, not permission to replace a
live call. In particular a single sampled append proves neither ownership nor
the capacity invariant for a later batch.
"""
from __future__ import annotations

import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from cpu_contract_events import event_records

U64 = 1 << 64

def _decode(records):
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    result = {}
    for record in records:
        code = bytes.fromhex(record['code'])
        instructions = list(decoder.disasm(code, record['rva']))
        if len(instructions) != 1 or instructions[0].size != len(code):
            raise ValueError('instruction_decode_mismatch')
        result[record['rva']] = (instructions[0], record)
    return result

def _reg(ins, operand):
    return ins.reg_name(operand.reg) if operand.type == X86_OP_REG else None

def _mem(ins, operand):
    if operand.type != X86_OP_MEM or operand.mem.index:
        return None
    return ins.reg_name(operand.mem.base), operand.mem.disp, operand.size

def _is_mov_load(ins, base):
    ops = ins.operands
    if ins.mnemonic != 'mov' or len(ops) != 2:
        return None
    dst = _reg(ins, ops[0])
    src = _mem(ins, ops[1])
    return (dst, src[1], src[2]) if dst and src and src[0] == base else None

def _is_mov_store(ins, base, offset, source):
    ops = ins.operands
    return (ins.mnemonic == 'mov' and len(ops) == 2 and
            _mem(ins, ops[0]) == (base, offset, 8) and
            _reg(ins, ops[1]) == source)

def derive_shape(records, entry_rva, end_rva):
    """Derive offsets/stride from decoded instructions, never a named struct."""
    decoded = _decode(r for r in records if entry_rva <= r['rva'] < end_rva)
    ordered = [(rva, decoded[rva][0]) for rva in sorted(decoded)]
    for i, (cmp_rva, compare) in enumerate(ordered):
        if compare.mnemonic != 'cmp' or len(compare.operands) != 2:
            continue
        cursor_reg = _reg(compare, compare.operands[0])
        capacity_reg = _reg(compare, compare.operands[1])
        if not cursor_reg or not capacity_reg or i + 1 >= len(ordered):
            continue
        branch_rva, branch = ordered[i + 1]
        if branch.mnemonic != 'je' or branch.operands[0].type != X86_OP_IMM:
            continue
        previous = ordered[max(0, i - 16):i]
        cursor_loads = [(rva, _is_mov_load(ins, 'rcx')) for rva, ins in previous]
        cursor_loads = [(rva, item) for rva, item in cursor_loads
                        if item and item[0] == cursor_reg and item[2] == 8]
        capacity_loads = [(rva, _is_mov_load(ins, 'rcx')) for rva, ins in previous]
        capacity_loads = [(rva, item) for rva, item in capacity_loads
                          if item and item[0] == capacity_reg and item[2] == 8]
        if not cursor_loads or not capacity_loads:
            continue
        cursor_offset = cursor_loads[-1][1][1]
        capacity_offset = capacity_loads[-1][1][1]
        fast = [(rva, ins) for rva, ins in ordered
                if branch_rva < rva < branch.operands[0].imm]
        zero_stores = [(rva, ins) for rva, ins in fast
                       if ins.mnemonic in ('vmovups', 'vmovdqu') and
                       ins.operands and _mem(ins, ins.operands[0]) ==
                       (cursor_reg, 0, ins.operands[0].size)]
        if len(zero_stores) != 1:
            continue
        zero_rva, zero_ins = zero_stores[0]
        zero_source = _reg(zero_ins, zero_ins.operands[1])
        zero_definitions = [
            (rva, ins) for rva, ins in fast if rva < zero_rva and
            ins.mnemonic in ('vpxor', 'vxorps') and len(ins.operands) == 3 and
            _reg(ins, ins.operands[0]) == _reg(ins, ins.operands[1]) ==
            _reg(ins, ins.operands[2])
        ]
        if not zero_source or not zero_definitions or (
                zero_source.replace('ymm', 'xmm') !=
                _reg(zero_definitions[-1][1], zero_definitions[-1][1].operands[0])):
            continue
        width = zero_ins.operands[0].size
        advances = []
        for rva, ins in fast:
            ops = ins.operands
            if ins.mnemonic != 'lea' or len(ops) != 2:
                continue
            result_reg = _reg(ins, ops[0])
            source = _mem(ins, ops[1])
            if not result_reg or not source or source[1] <= 0:
                continue
            if not any(_is_mov_store(next_ins, 'rdi', cursor_offset, result_reg)
                       for next_rva, next_ins in fast if next_rva > rva):
                continue
            if not any(_is_mov_load(prior_ins, 'rdi') ==
                       (source[0], cursor_offset, 8)
                       for prior_rva, prior_ins in fast if prior_rva < rva):
                continue
            advances.append((rva, source[1]))
        if len(advances) != 1 or width != advances[0][1]:
            continue
        return {
            'cursor_offset': cursor_offset,
            'capacity_offset': capacity_offset,
            'record_stride': advances[0][1],
            'cursor_load_rva': cursor_loads[-1][0],
            'capacity_load_rva': capacity_loads[-1][0],
            'comparison_rva': cmp_rva,
            'growth_branch_rva': branch_rva,
            'growth_target_rva': branch.operands[0].imm,
            'zero_store_rva': zero_rva,
            'cursor_advance_rva': advances[0][0],
        }
    raise ValueError('no_byte_verified_no_growth_append_shape')

def derive_append_contract(path):
    path = Path(path)
    meta = json.loads((path / 'capture.json').read_text())
    plan = json.loads((path / 'memory-plan.json').read_text())
    events = list(event_records(path, meta, vectors=True))
    if not meta.get('callee_mode') or not events or not meta.get('completed_calls'):
        raise ValueError('not_a_completed_direct_callee_sample')
    entry_rva = meta['entry'] - meta['main_base']
    end_rva = meta['entry_end'] - meta['main_base']
    shape = derive_shape(plan['records'], entry_rva, end_rva)
    decoded = _decode(r for r in plan['records'] if entry_rva <= r['rva'] < end_rva)
    for event in events:
        rva = event['rip'] - meta['main_base']
        if rva in decoded:
            ins, record = decoded[rva]
            if event['code'][:2 * ins.size] != record['code']:
                raise ValueError('captured_code_disagrees_with_memory_plan')
    by_rva = {e['rip'] - meta['main_base']: e for e in events}
    compare = by_rva.get(shape['comparison_rva'])
    branch = by_rva.get(shape['growth_branch_rva'])
    zero = by_rva.get(shape['zero_store_rva'])
    if not compare or not branch or not zero:
        raise ValueError('fast_path_not_observed')
    if (branch['index'] + 1 >= len(events) or
            events[branch['index'] + 1]['rip'] - meta['main_base'] !=
            shape['growth_branch_rva'] +
            decoded[shape['growth_branch_rva']][0].size):
        # The next PC proves the equality branch was not taken.
        raise ValueError('growth_branch_or_unexpected_fast_path')
    # Windows x64 context register order in cpu_contract_events.
    owner = events[0]['registers'][1]
    cursor = compare['registers'][6]
    capacity = compare['registers'][1]
    stride = shape['record_stride']
    for load_name, offset, expected in (
        ('cursor_load_rva', shape['cursor_offset'], cursor),
        ('capacity_load_rva', shape['capacity_offset'], capacity),
    ):
        loaded = by_rva.get(shape[load_name])
        if (not loaded or len(loaded.get('memory', [])) != 1 or
                loaded['memory'][0]['address'] != owner + offset or
                not loaded['memory'][0]['ok'] or
                int.from_bytes(bytes.fromhex(loaded['memory'][0]['bytes']), 'little') !=
                expected):
            raise ValueError('header_load_does_not_match_comparison')
    if cursor == capacity or cursor + stride > U64 or cursor + stride > capacity:
        raise ValueError('sample_does_not_fit_no_growth_path')
    if zero['memory'][0]['address'] != cursor or zero['memory'][0]['size'] != stride:
        raise ValueError('zero_store_address_or_width_mismatch')
    header_address = owner + shape['cursor_offset']
    observed_writes = []
    for event in events:
        rva = event['rip'] - meta['main_base']
        item = decoded.get(rva)
        if not item:
            continue
        ins, record = item
        for operand_index, (operand, memory) in enumerate(zip(record['operands'], event.get('memory', []))):
            # Some decoder maps mark vpextrw's memory destination as a read.
            # Its decoded first operand is unambiguously a store.
            decoded_store = (ins.mnemonic == 'vpextrw' and
                             operand_index == 0 and
                             ins.operands[0].type == X86_OP_MEM)
            if not operand.get('write') and not decoded_store:
                continue
            address, size = memory['address'], memory['size']
            if not memory['ok'] or size != operand['size']:
                raise ValueError('unverified_write')
            if cursor <= address and address + size <= cursor + stride:
                observed_writes.append({
                    'rva': rva, 'offset': address - cursor, 'width': size,
                    'operation': ins.mnemonic, 'source': ins.op_str,
                })
            elif address == header_address and size == 8:
                observed_writes.append({
                    'rva': rva, 'target': 'cursor_header', 'width': size,
                    'operation': ins.mnemonic, 'source': ins.op_str,
                })
            elif not (meta['stack_low'] <= address and
                      address + size <= meta['stack_high']):
                raise ValueError('write_outside_record_header_or_stack')
    if not observed_writes or observed_writes[0]['rva'] != shape['zero_store_rva']:
        raise ValueError('missing_first_record_zero')
    if sum(w.get('target') == 'cursor_header' for w in observed_writes) != 1:
        raise ValueError('missing_or_conflicting_cursor_update')
    if any(w.get('offset', 0) + w['width'] > stride for w in observed_writes):
        raise ValueError('record_write_exceeds_stride')
    register_copies = []
    stack_reads = []
    for rva, (ins, record) in sorted(decoded.items()):
        if rva not in by_rva or len(ins.operands) < 2:
            continue
        dest = _reg(ins, ins.operands[0])
        source = _reg(ins, ins.operands[1])
        if dest and source and ins.mnemonic in ('mov', 'vmovaps'):
            register_copies.append({'rva': rva, 'destination': dest, 'source': source})
        stack = _mem(ins, ins.operands[1])
        if dest and stack and stack[0] == 'rsp':
            stack_reads.append({'rva': rva, 'stack_offset_at_instruction': stack[1],
                                'width': stack[2], 'destination': dest})
    return {
        'schema': 1,
        'status': 'isolated_no_growth_sample',
        'source_sha256': plan.get('source_sha256'),
        'entry_rva': entry_rva,
        'verified_instruction_bytes': {
            str(e['rip'] - meta['main_base']):
                decoded[e['rip'] - meta['main_base']][1]['code']
            for e in events if e['rip'] - meta['main_base'] in decoded
        },
        'container': {'cursor_offset': shape['cursor_offset'],
                      'capacity_offset': shape['capacity_offset'],
                      'cursor_is_end_pointer': True},
        'record_stride': stride,
        'machine_path': shape,
        'sample': {'owner_address': owner, 'old_cursor': cursor,
                   'capacity_end': capacity, 'record_count': 1,
                   'scope': 'one_completed_callee_invocation'},
        'ordered_writes': observed_writes,
        'source_provenance': {
            'entry_abi': 'Windows x64 register and caller-stack arguments',
            'register_copies': register_copies,
            'stack_reads': stack_reads,
            'record_write_expressions': [w['source'] for w in observed_writes
                                         if 'offset' in w],
            'limitations': 'register and stack value algebra, all-input FP16 rounding, and source range remain open',
        },
        'preflight_obligations': [
            'isolated_snapshot_contiguous_bounds',
            'cursor_and_capacity_alignment',
            'count_times_stride_fits_without_growth',
            'nonaliasing_sources_headers_and_outputs',
            'all_iterations_same_path_and_equivalent_values',
        ],
        'unknown': [
            'live_memory_exclusive_ownership_and_allocation_generation',
            'growth_branch_behavior',
            'global_input_range_and_append_count',
            'source_aliases_and_inter_iteration_liveness',
            'external_consumer_and_publication_deadline',
            'exact_FP16_lowering_for_all_inputs',
        ],
        'replacement_admitted': False,
    }

def preflight_no_growth(contract, *, begin, cursor, capacity, count,
                        snapshot_bounds, source_ranges=(), header_range=None):
    """Bounds check for a caller-owned isolated snapshot; never dereferences live memory."""
    if not isinstance(snapshot_bounds, (tuple, list)) or len(snapshot_bounds) != 2:
        return {'input_guards_pass': False, 'reason': 'invalid_integer_or_bounds',
                'replacement_admitted': False}
    stride = contract.get('record_stride')
    values = (begin, cursor, capacity, count, stride, *snapshot_bounds)
    if (any(type(v) is not int for v in values) or
            any(v < 0 or v >= U64 for v in values) or stride <= 0 or count < 0):
        return {'input_guards_pass': False, 'reason': 'invalid_integer_or_bounds',
                'replacement_admitted': False}
    lo, hi = snapshot_bounds
    end = cursor + count * stride
    if not (lo <= begin <= cursor <= capacity <= hi < U64) or end >= U64:
        reason = 'outside_snapshot_or_overflow'
    elif (cursor - begin) % stride or (capacity - begin) % stride:
        reason = 'misaligned_cursor_or_capacity'
    elif end > capacity:
        reason = 'would_take_growth_branch'
    elif header_range is None:
        reason = 'header_range_not_provided'
    elif header_range is not None and _overlap((cursor, end), header_range):
        reason = 'header_aliases_output'
    elif any(_overlap((cursor, end), span) for span in source_ranges):
        reason = 'source_aliases_output'
    else:
        reason = None
    return {'input_guards_pass': reason is None, 'reason': reason,
            'isolated_output_end': end if reason is None else None,
            'replacement_admitted': False}

def preflight_packet_no_growth(contract, *, packet_count, **snapshot):
    """Reserve for every possible emit; predicate results need not be known yet."""
    result = preflight_no_growth(contract, count=packet_count, **snapshot)
    result['max_possible_outputs'] = packet_count
    result['capacity_policy'] = 'reserve_packet_count_times_stride'
    return result

def _captured_value(event, address, size, *, after=False):
    items = event.get('previous_after', ()) if after else event.get('memory', ())
    for item in items:
        if (item['address'] == address and item['size'] == size and
                item.get('ok') and len(item['bytes']) == 2 * size):
            return bytes.fromhex(item['bytes'])
    return None

def compose_append_events(events, main_base, contract, *, initial_cursor=None):
    """Classify bounded iteration events using a separately derived callee shape.

    Absolute addresses are evidence checks only. GPU output slots are the
    prefix sum of emit flags; no CPU-computed cursor sequence is an input.
    """
    shape = contract['machine_path']
    stride = contract['record_stride']
    entry_rva = contract['entry_rva']
    iterations = []
    current = None
    for event in events:
        if event['kind'] == 1:
            if current is not None:
                raise ValueError('nested_or_unclosed_iteration')
            current = []
        elif event['kind'] == 2:
            if current is None:
                raise ValueError('iteration_exit_without_entry')
            iterations.append(current)
            current = None
        elif current is not None:
            current.append(event)
    if current is not None or not iterations:
        raise ValueError('missing_completed_iterations')

    rows = []
    emit_flags = []
    owner = None
    prior_cursor = initial_cursor
    initial_observed = None
    capacity_end = None
    byte_verified = True
    final_header_after_verified = False
    for iteration_index, trace in enumerate(iterations):
        calls = [i for i, e in enumerate(trace) if e['rip'] - main_base == entry_rva]
        if len(calls) > 1:
            raise ValueError('multiple_appends_in_one_iteration')
        if not calls:
            emit_flags.append(0)
            continue
        emit_flags.append(1)
        start = calls[0]
        call = trace[start:]
        for event in call:
            rva = event['rip'] - main_base
            expected_code = contract.get('verified_instruction_bytes', {}).get(str(rva))
            if expected_code and not event.get('code', '').startswith(expected_code):
                raise ValueError('callee_machine_bytes_changed')
        by_rva = {e['rip'] - main_base: (i, e) for i, e in enumerate(call)}
        required = (shape['cursor_load_rva'], shape['capacity_load_rva'],
                    shape['growth_branch_rva'], shape['zero_store_rva'])
        if any(rva not in by_rva for rva in required):
            raise ValueError('incomplete_callee_fast_path')
        call_owner = call[0]['registers'][1]
        if owner is None:
            owner = call_owner
        elif owner != call_owner:
            raise ValueError('append_header_changed')
        cursor_load = by_rva[shape['cursor_load_rva']][1]
        capacity_load = by_rva[shape['capacity_load_rva']][1]
        cursor_raw = _captured_value(
            cursor_load, owner + shape['cursor_offset'], 8)
        capacity_raw = _captured_value(
            capacity_load, owner + shape['capacity_offset'], 8)
        if cursor_raw is None or capacity_raw is None:
            raise ValueError('missing_header_read')
        old_cursor = int.from_bytes(cursor_raw, 'little')
        if initial_observed is None:
            initial_observed = old_cursor
        capacity = int.from_bytes(capacity_raw, 'little')
        if prior_cursor is not None and old_cursor != prior_cursor:
            raise ValueError('cursor_chain_discontinuity')
        if capacity_end is None:
            capacity_end = capacity
        elif capacity != capacity_end:
            raise ValueError('capacity_changed_across_packet')
        if (old_cursor >= U64 or old_cursor + stride >= U64 or
                old_cursor + stride > capacity):
            raise ValueError('callee_would_require_growth')
        branch_i, branch = by_rva[shape['growth_branch_rva']]
        if (branch_i + 1 >= len(call) or
                call[branch_i + 1]['rip'] - main_base ==
                shape['growth_target_rva']):
            raise ValueError('growth_branch_taken')
        record = bytearray(stride)
        known = [False] * stride
        new_cursor = old_cursor + stride
        header_seen = False
        for write in contract['ordered_writes']:
            rva = write['rva']
            if rva not in by_rva:
                raise ValueError('missing_append_write')
            i, event = by_rva[rva]
            if i + 1 >= len(call):
                raise ValueError('missing_postwrite_event')
            if write.get('target') == 'cursor_header':
                address = owner + shape['cursor_offset']
            else:
                address = old_cursor + write['offset']
            size = write['width']
            if _captured_value(event, address, size) is None:
                raise ValueError('append_write_address_mismatch')
            after = _captured_value(call[i + 1], address, size, after=True)
            if write.get('target') == 'cursor_header':
                header_seen = True
                if after is not None and int.from_bytes(after, 'little') != new_cursor:
                    raise ValueError('cursor_advance_mismatch')
                final_header_after_verified = after is not None
            else:
                if after is None:
                    byte_verified = False
                else:
                    offset = write['offset']
                    record[offset:offset + size] = after
                    known[offset:offset + size] = [True] * size
        if not header_seen:
            raise ValueError('cursor_header_not_written')
        output_slot = len(rows)
        rows.append({
            'iteration_index': iteration_index,
            'output_slot': output_slot,
            'observed_record_address': old_cursor,
            'bytes_hex': record.hex() if all(known) else None,
            'byte_verified': all(known),
            'cursor_header_after_verified': final_header_after_verified,
        })
        prior_cursor = new_cursor
    append_count = len(rows)
    return {
        'schema': 1,
        'status': 'sampled_packet_composition',
        'iteration_count': len(iterations),
        'emit_flags': emit_flags,
        'append_count': append_count,
        'record_stride': stride,
        'initial_cursor_observed': initial_observed,
        'initial_cursor_input': initial_cursor,
        'final_cursor_expected': prior_cursor,
        'final_cursor_observed': prior_cursor if (append_count and
                                                final_header_after_verified) else None,
        'capacity_end_observed': capacity_end,
        'container_address_observed': owner,
        'records': rows,
        'all_record_bytes_verified': byte_verified,
        'gpu_compaction': {
            'strategy': 'exclusive_prefix_sum_then_scatter',
            'output_slot_source': 'GPU prefix of emit flags',
            'max_output_count_for_preflight': len(iterations),
            'final_cursor': 'initial_cursor + popcount(emit_flags) * record_stride',
        },
        'scope': 'observed_completed_iterations_only',
        'replacement_admitted': False,
    }

def compose_append_trace(path, contract, *, initial_cursor=None):
    path = Path(path)
    meta = json.loads((path / 'capture.json').read_text())
    if not meta.get('iteration_mode') or not meta.get('memory_capture_enabled'):
        raise ValueError('not_an_iteration_memory_capture')
    events = list(event_records(path, meta, vectors=False))
    if len(events) != meta['written_events']:
        raise ValueError('truncated_iteration_capture')
    return compose_append_events(events, meta['main_base'], contract,
                                 initial_cursor=initial_cursor)

def _overlap(a, b):
    if (len(b) != 2 or any(type(x) is not int for x in b) or b[0] < 0 or
            b[1] < b[0] or b[1] >= U64):
        return True
    return a[0] < b[1] and b[0] < a[1]
