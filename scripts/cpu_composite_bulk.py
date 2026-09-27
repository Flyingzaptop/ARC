"""Bounded packet preflight, code proof, and snapshot fixture binding."""
import hashlib
import json
import shutil
import struct
import tempfile
from pathlib import Path
import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
from cpu_composite_shared import Unsupported
from cpu_composite_gpu import generate
from cpu_evidence_paths import artifact_path, capture_path

def _packet_guard_filter(models, append_contract, packet_preflight, *, allow_unbounded_sources=False):
    if packet_preflight is None:
        return None, {'mode': 'captured_per_row_live_ins',
                      'growth_guard': 'per_row', 'terminal_guard': 'per_row'}
    from cpu_append_contract import preflight_packet_no_growth
    loop_proof = packet_preflight.get('loop_bound_proof') or {}
    packet_count = loop_proof.get('count', len(models))
    if not isinstance(packet_count, int) or packet_count < len(models) or packet_count > 65536:
        raise Unsupported('loop count proof outside packet budget')
    if (packet_preflight.get('status') != 'verified_on_owned_before_view' or
            packet_preflight.get('max_count') != packet_count or
            packet_preflight.get('record_stride') != append_contract.get('record_stride')):
        raise Unsupported('packet preflight identity or size mismatch')
    if packet_count > 1 and not packet_preflight.get('source_ranges') and not allow_unbounded_sources:
        raise Unsupported('packet source ranges not bounded')
    snapshot = {k: packet_preflight[k] for k in
                ('begin', 'cursor', 'capacity', 'snapshot_bounds', 'source_ranges', 'header_range')}
    result = preflight_packet_no_growth(append_contract, packet_count=packet_count, **snapshot)
    if not result['input_guards_pass']:
        raise Unsupported('whole-packet no-growth preflight: ' + str(result['reason']))
    typed = models[0]['typed_ir']
    nodes = typed['nodes']
    container = append_contract['container']
    growth = []
    for guard in typed.get('guards', []):
        source = guard.get('source')
        if guard['predicate'] not in ('je', 'jne') or not isinstance(source, int):
            continue
        compare = nodes[source]
        if compare['op'] != 'cmp' or compare['type'] != 'u64' or len(compare['inputs']) != 2:
            continue
        leaves = [nodes[i] for i in compare['inputs'] if isinstance(i, int)]
        if len(leaves) != 2 or any(leaf['op'] != 'memory_input' for leaf in leaves):
            continue
        recipes = [leaf.get('address_recipe') or {} for leaf in leaves]
        if (recipes[0].get('base') == recipes[1].get('base') and
                recipes[0].get('index') == recipes[1].get('index') and
                [recipe.get('disp') for recipe in recipes] ==
                [container['cursor_offset'], container['capacity_offset']]):
            if guard['taken']:
                raise Unsupported('captured growth branch conflicts with no-growth preflight')
            growth.append(source)
    if len(growth) != 1:
        raise Unsupported('one structural cursor/capacity growth guard required')
    skipped = set(growth)
    terminal = 'per_row_unproven'
    if loop_proof.get('status') == 'verified_counted_tail':
        step = loop_proof.get('step')
        tail = loop_proof.get('tail') or []
        if (not isinstance(step, int) or step <= 0 or
                loop_proof.get('entry_value', 0) + packet_count * step != loop_proof.get('bound_value') or
                loop_proof.get('exit_value') != loop_proof.get('bound_value') or
                [entry.get('mnemonic') for entry in tail] != ['add', 'cmp', 'jne'] or
                any(not isinstance(entry.get('rva'), int) or not entry.get('code') for entry in tail)):
            raise Unsupported('counted tail proof arithmetic or machine-code shape')
        candidate = capture_path(loop_proof.get('candidate', ''))
        source = candidate/'expected-code.bin'
        if (not source.is_file() or
                hashlib.sha256(source.read_bytes()).hexdigest() != loop_proof.get('expected_code_sha256')):
            raise Unsupported('counted tail expected-code identity')
        request = (candidate/'request.txt').read_text().split()
        if len(request) < 2:
            raise Unsupported('counted tail capture request missing')
        expected_start, expected_size = int(request[0], 16), int(request[1], 16)
        expected_code = source.read_bytes()
        if expected_size != len(expected_code):
            raise Unsupported('counted tail expected-code size differs from request')
        from cpu_capture_memory import checked_functions
        checked = checked_functions(candidate/'checked-functions.bin')
        capture = json.loads((candidate/'capture.json').read_text())
        base = capture['main_base']
        decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        decoder.detail = True
        decoded = []
        for entry in tail:
            raw = bytes.fromhex(entry['code'])
            offset = entry['rva'] - expected_start
            if offset < 0 or expected_code[offset:offset+len(raw)] != raw:
                raise Unsupported('counted tail differs from expected-code capture')
            if not any(start <= entry['rva'] and entry['rva']+len(raw) <= start+len(code) and
                       code[entry['rva']-start:entry['rva']-start+len(raw)] == raw
                       for start, code in checked):
                raise Unsupported('counted tail differs from checked machine code')
            instruction = next(decoder.disasm(raw, base+entry['rva'], count=1), None)
            if instruction is None or instruction.bytes != raw or instruction.mnemonic != entry['mnemonic']:
                raise Unsupported('counted tail decode differs from proof')
            decoded.append(instruction)
        add, compare, branch = decoded
        if (len(add.operands) != 2 or add.operands[0].type != X86_OP_REG or
                add.reg_name(add.operands[0].reg) != loop_proof.get('register') or
                add.operands[1].type != X86_OP_IMM or add.operands[1].imm != step or
                len(compare.operands) != 2 or compare.operands[0].type != X86_OP_REG or
                compare.reg_name(compare.operands[0].reg) != loop_proof.get('register') or
                compare.operands[1].type != X86_OP_MEM or
                len(branch.operands) != 1 or branch.operands[0].type != X86_OP_IMM or
                branch.operands[0].imm != loop_proof.get('entry_rip') or
                branch.address+branch.size != loop_proof.get('exit_rip')):
            raise Unsupported('counted tail register, step, or branch boundary')
        last = typed.get('guards', [])[-1]
        source = last.get('source')
        if (last['predicate'] != 'jne' or not isinstance(source, int) or
                nodes[source]['op'] != 'cmp' or nodes[source]['type'] != 'u64'):
            raise Unsupported('terminal induction proof does not match guard shape')
        skipped.add(source)
        terminal = 'removed_by_checked_counted_tail_proof'
    return skipped, {'mode': 'isolated_positive_packet_preflight', 'growth_guard': 'removed_by_max_count_preflight',
                     'terminal_guard': terminal, 'initial_cursor': packet_preflight['cursor'],
                     'capacity_end': packet_preflight['capacity'], 'loop_count': packet_count,
                     'source_alias_status': 'unverified_static_plan_only' if allow_unbounded_sources else 'bounded_source_ranges',
                     'preflight_result': result}


def generate_packet_shader_plan(models, append_contract, packet_proof, output):
    """Emit shader text and ordinal slots before snapshot gather is bound.

    A no-growth check may justify removing the cursor/capacity branch, but
    source aliasing and input freshness remain unresolved. This writes no
    executable input or expected-output buffers.
    """
    if packet_proof.get('status') != 'counted_tail_and_no_growth_snapshot_verified':
        raise Unsupported('counted packet proof missing')
    preflight = dict(packet_proof['packet_preflight'])
    preflight['loop_bound_proof'] = packet_proof['loop_bound_proof']
    preflight['source_ranges'] = []
    with tempfile.TemporaryDirectory() as temporary:
        sample_folder = Path(temporary)/'sample'
        sample = generate(models, append_contract, sample_folder,
                          packet_preflight=preflight, allow_unbounded_sources=True)
        output = Path(output)
        output.mkdir(parents=True, exist_ok=False)
        shutil.copyfile(sample_folder/'generated.hlsl', output/'generated.hlsl')
        contract = dict(sample)
        contract.update(count=packet_proof['loop_bound_proof']['count'],
                        status='static_shader_plan_binding_incomplete',
                        input_sha256=None, expected_sha256=None,
                        independent_record_sha256=None,
                        traced_rows_checked=len(models),
                        input_buffers_emitted=False, expected_buffers_emitted=False,
                        executable_gpu_trial_allowed=False,
                        packet_preparation='native gather and source-range audit pending',
                        limitations=sample['limitations'] +
                        ['source alias ranges, packed input bytes, and independent after-view record binding pending'])
        (output/'contract.json').write_text(json.dumps(contract, indent=2) + '\n', encoding='utf-8')
        return contract


def generate_bulk_fixture(models, append_contract, packet_proof, packed_input,
                          expected_records, output):
    """Bind a proved positive packet to snapshot-gathered rows for GPU trial.

    Two traced rows check the gathered slot order. The remaining expected
    record bytes must come from an independent after-view, never generated from
    the shader or copied from CPU prefix positions as input slots.
    """
    preflight = dict(packet_proof['packet_preflight'])
    preflight.update(packet_proof.get('packet_preflight_args') or {})
    preflight['loop_bound_proof'] = packet_proof['loop_bound_proof']
    count = preflight['loop_bound_proof']['count']
    append_result = packet_proof.get('append_preflight_result') or {}
    binding = packet_proof.get('artifact_binding') or {}
    if (count < 2 or len(models) < 2 or
            packet_proof.get('status') not in ('counted_tail_and_no_growth_snapshot_verified',
                                               'verified_bulk_packet') or
            not append_result.get('input_guards_pass') or
            append_result.get('max_possible_outputs') != count or
            not preflight.get('source_ranges')):
        raise Unsupported('bulk packet proof or traced rows missing')
    input_path, record_path = artifact_path(packed_input), artifact_path(expected_records)
    packed_input = input_path.read_bytes()
    expected_records = record_path.read_bytes()
    independent = packet_proof.get('independent_after_view') or {}
    if (independent.get('record_bytes') != len(expected_records) or
            independent.get('record_sha256') != hashlib.sha256(expected_records).hexdigest() or
            independent.get('final_cursor') != preflight['cursor'] + count * append_contract['record_stride']):
        raise Unsupported('independent after-view record binding or final cursor differs')
    for kind, path, raw, source in (
            ('packed_input', input_path, packed_input, 'bounded_native_before_view_gather'),
            ('expected_records', record_path, expected_records, 'independent_after_view_capture')):
        item = binding.get(kind) or {}
        if (artifact_path(item.get('path', '')).resolve() != path.resolve() or item.get('bytes') != len(raw) or
                item.get('sha256') != hashlib.sha256(raw).hexdigest() or item.get('source') != source):
            raise Unsupported('bulk artifact binding differs: ' + kind)
    stride = append_contract['record_stride']
    if len(expected_records) != count * stride:
        raise Unsupported('independent after-view record extent differs from counted tail')
    with tempfile.TemporaryDirectory() as temporary:
        sample_folder = Path(temporary)/'sample'
        sample = generate(models, append_contract, sample_folder,
                          packet_preflight=preflight)
        input_stride = sample['input_words_per_call'] * 4
        output_stride = sample['output_words_per_call'] * 4
        if len(packed_input) != count * input_stride:
            raise Unsupported('snapshot-gathered input extent differs from normalized shader slots')
        traced_input = (sample_folder/'input.bin').read_bytes()
        if packed_input[:len(traced_input)] != traced_input:
            raise Unsupported('bulk gather first traced rows differ from machine-code replay')
        traced_expected = (sample_folder/'expected.bin').read_bytes()
        for i in range(len(models)):
            if expected_records[i*stride:(i+1)*stride] != traced_expected[i*output_stride+4:i*output_stride+4+stride]:
                raise Unsupported('bulk after-view first traced records differ from replay')
        output = Path(output)
        output.mkdir(parents=True, exist_ok=False)
        shutil.copyfile(sample_folder/'generated.hlsl', output/'generated.hlsl')
        (output/'input.bin').write_bytes(packed_input)
        expected = b''.join(struct.pack('<I', 1) +
                            expected_records[i*stride:(i+1)*stride] +
                            struct.pack('<Q', stride) for i in range(count))
        (output/'expected.bin').write_bytes(expected)
        contract = dict(sample)
        contract.update(status='isolated_bulk_fixture_bound', count=count,
                        input_sha256=hashlib.sha256(packed_input).hexdigest(),
                        expected_sha256=hashlib.sha256(expected).hexdigest(),
                        independent_record_sha256=hashlib.sha256(expected_records).hexdigest(),
                        scope='isolated positive bulk packet from before-view inputs and independent after-view records',
                        packet_preparation='bounded CPU snapshot gather; cost must be measured and included',
                        full_batch_replacement_allowed=False,
                        traced_rows_checked=len(models),
                        source_ranges_status=packet_proof.get('source_ranges_status'),
                        artifact_binding=binding,
                        all_rows_positive_assumption=True)
        (output/'contract.json').write_text(json.dumps(contract, indent=2) + '\n', encoding='utf-8')
        return contract
