import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from cpu_composite_gpu import (Unsupported, _half_rne_bits, compaction_reference,
                               generate, generate_compaction,
                               generate_packet_shader_plan, generate_bulk_fixture)

EVIDENCE_OVERRIDE=os.environ.get('ARC_COMPOSITE_EVIDENCE_ROOT')
BASE=Path(EVIDENCE_OVERRIDE) if EVIDENCE_OVERRIDE else Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer')
ARTIFACT_OVERRIDE=os.environ.get('ARC_COMPOSITE_ARTIFACT_ROOT')
ARTIFACT_BASE=Path(ARTIFACT_OVERRIDE) if ARTIFACT_OVERRIDE else ROOT/'build/cpu-ir-gpu'


def require_capture(case,path):
    if path.exists():return
    message=f'missing {path}; set ARC_COMPOSITE_EVIDENCE_ROOT to the extracted capture root'
    if EVIDENCE_OVERRIDE:case.fail(message)
    case.skipTest(message)


def require_artifact(case,path):
    if path.exists():return
    message=f'missing {path}; set ARC_COMPOSITE_ARTIFACT_ROOT to extracted artifacts/cpu-ir-gpu'
    if ARTIFACT_OVERRIDE or EVIDENCE_OVERRIDE:case.fail(message)
    case.skipTest(message)


def sample():
    record = bytes(range(16))
    cursor = struct.pack('<Q', 0x1010)
    model = {
        'status': 'closed_observed_path',
        'typed_ir': {
            'schema': 1, 'missing': ['live ownership unproven'],
            'nodes': [
                {'id': 0, 'op': 'entry_vector', 'type': 'u32x4', 'inputs': [],
                 'name': 0, 'bytes': record.hex(), 'slot': None},
                {'id': 1, 'op': 'entry_register', 'type': 'u64', 'inputs': [],
                 'name': 'rbx', 'bytes': cursor.hex(), 'slot': None},
            ],
            'outputs': [
                {'address': 0x1000, 'width': 16, 'bytes': record.hex(), 'source': 0, 'slot': None},
                {'address': 0x3000, 'width': 8, 'bytes': cursor.hex(), 'source': 1, 'slot': None},
            ],
            'guards': [],
        },
    }
    append = {
        'status': 'isolated_no_growth_sample', 'record_stride': 16,
        'container': {'cursor_is_end_pointer': True},
        'sample': {'old_cursor': 0x1000},
        'ordered_writes': [
            {'offset': 0, 'width': 16},
            {'target': 'cursor_header', 'width': 8},
        ],
    }
    return model, append


class CompositeGpuTests(unittest.TestCase):
    def test_explicit_half_rounding_matches_cpu_nearest_even(self):
        values = [0.0, -0.0, 2**-25, 2**-24, 2**-14, 1.0,
                  1.0 + 2**-11, 1.0 + 3 * 2**-11,
                  5.5875115394592285, 6.03906774520874,
                  11.279224395751953, 12.256467819213867,
                  65504.0, -7.9038310050964355]
        for value in values:
            bits = struct.unpack('<I', struct.pack('<f', value))[0]
            expected = struct.unpack('<H', struct.pack('<e', value))[0]
            self.assertEqual(_half_rne_bits(bits), expected, value)

    def test_scratch_record_and_cursor_delta_are_complete(self):
        model, append = sample()
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / 'fixture'
            contract = generate(model, append, folder)
            self.assertEqual((contract['count'], contract['input_words_per_call'],
                              contract['output_words_per_call']), (1, 4, 7))
            self.assertEqual((folder / 'expected.bin').read_bytes(),
                             struct.pack('<I', 1) + bytes(range(16)) + struct.pack('<Q', 16))
            self.assertEqual(contract['pruned_pointer_nodes'], [1])
            self.assertFalse(contract['replacement_allowed'])
            self.assertEqual(contract['shader_sha256'],
                             hashlib.sha256((folder / 'generated.hlsl').read_bytes()).hexdigest())
            self.assertIn('Outputs.Store(outBase', (folder / 'generated.hlsl').read_text())

    def test_pack_bytes_lowers_observed_lane_selection(self):
        model, append = sample()
        first = bytes(range(4)) + bytes(12)
        refs = [{'id': 0, 'byte_offset': i} if i < 4 else
                {'id': 1, 'byte_offset': i} for i in range(16)]
        model['typed_ir']['nodes'] = [
            {'id': 0, 'op': 'entry_vector', 'type': 'u32x4', 'inputs': [], 'bytes': bytes(range(16)).hex()},
            {'id': 1, 'op': 'entry_vector', 'type': 'u32x4', 'inputs': [], 'bytes': bytes(16).hex()},
            {'id': 2, 'op': 'pack_bytes', 'type': 'u32x4', 'inputs': refs, 'bytes': first.hex()},
            {'id': 3, 'op': 'entry_register', 'type': 'u64', 'inputs': [],
             'bytes': struct.pack('<Q', 0x1010).hex()},
        ]
        model['typed_ir']['outputs'][0]['bytes'] = first.hex()
        model['typed_ir']['outputs'][0]['source'] = 2
        model['typed_ir']['outputs'][1]['source'] = 3
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / 'fixture'
            contract = generate(model, append, folder)
            self.assertEqual(contract['input_words_per_call'], 8)
            self.assertEqual((folder / 'expected.bin').read_bytes()[4:20], first)
            self.assertIn('uint4 v2', (folder / 'generated.hlsl').read_text())

    def test_missing_output_or_wrong_cursor_fails_closed(self):
        model, append = sample()
        model['typed_ir']['outputs'].pop()
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(Unsupported, 'footprint'):
                generate(model, append, Path(temp) / 'fixture')
        model, append = sample()
        model['typed_ir']['outputs'][1]['bytes'] = struct.pack('<Q', 0x1011).hex()
        model['typed_ir']['nodes'][1]['bytes'] = struct.pack('<Q', 0x1011).hex()
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(Unsupported, 'cursor advance'):
                generate(model, append, Path(temp) / 'fixture')

    def test_packet_requires_same_typed_topology(self):
        model, append = sample()
        other = json.loads(json.dumps(model))
        with tempfile.TemporaryDirectory() as temp:
            contract = generate([model, other], append, Path(temp) / 'fixture')
            self.assertEqual(contract['count'], 2)
        other['typed_ir']['nodes'][0]['op'] = 'immediate'
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(Unsupported, 'topology'):
                generate([model, other], append, Path(temp) / 'fixture')

    def test_packet_preflight_prunes_structural_growth_guard(self):
        model, append = sample()
        append['container'].update(cursor_offset=8, capacity_offset=16)
        nodes = model['typed_ir']['nodes']
        nodes.extend([
            {'id': 2, 'op': 'memory_input', 'type': 'u64', 'inputs': [],
             'bytes': struct.pack('<Q', 0x1000).hex(),
             'address_recipe': {'base': 'rcx', 'index': None, 'disp': 8}},
            {'id': 3, 'op': 'memory_input', 'type': 'u64', 'inputs': [],
             'bytes': struct.pack('<Q', 0x2000).hex(),
             'address_recipe': {'base': 'rcx', 'index': None, 'disp': 16}},
            {'id': 4, 'op': 'cmp', 'type': 'u64', 'inputs': [2, 3],
             'bytes': struct.pack('<Q', (0x1000 - 0x2000) % (1 << 64)).hex()},
        ])
        model['typed_ir']['guards'] = [{'predicate': 'je', 'taken': False, 'source': 4}]
        preflight = {'status': 'verified_on_owned_before_view', 'max_count': 1,
                     'record_stride': 16, 'begin': 0x1000, 'cursor': 0x1000, 'capacity': 0x2000,
                     'snapshot_bounds': [0x1000, 0x4000], 'source_ranges': [],
                     'header_range': [0x3000, 0x3018]}
        with tempfile.TemporaryDirectory() as temp:
            contract = generate(model, append, Path(temp) / 'fixture', packet_preflight=preflight)
            self.assertEqual(contract['input_words_per_call'], 4)
            self.assertEqual(contract['packet_scope']['growth_guard'], 'removed_by_max_count_preflight')
            self.assertNotIn(2, contract['reachable_nodes'])
        nodes.append({'id': 5, 'op': 'immediate', 'type': 'u64', 'inputs': [],
                      'value': 8, 'bytes': struct.pack('<Q', 8).hex()})
        nodes.append({'id': 6, 'op': 'cmp', 'type': 'u64', 'inputs': [2, 5],
                      'bytes': struct.pack('<Q', 0x1000 - 8).hex()})
        model['typed_ir']['guards'].append({'predicate': 'jne', 'taken': True, 'source': 6})
        with tempfile.TemporaryDirectory() as temp:
            candidate = Path(temp) / 'candidate'
            candidate.mkdir()
            tail = [('add', 1, '4983c404'), ('cmp', 5, '4c3ba42490000000'),
                    ('jne', 13, '0f85a3feffff')]
            code = b''.join(bytes.fromhex(item[2]) for item in tail)
            (candidate / 'expected-code.bin').write_bytes(code)
            (candidate / 'request.txt').write_text(f'1 {len(code):x}\n')
            (candidate / 'checked-functions.bin').write_bytes(struct.pack('<III', 1, 1, len(code)) + code)
            (candidate / 'capture.json').write_text(json.dumps({'main_base': 0x1000}))
            preflight['loop_bound_proof'] = {
                'status': 'verified_counted_tail', 'register': 'r12', 'count': 1, 'step': 4,
                'entry_value': 4, 'bound_value': 8, 'exit_value': 8,
                'entry_rip': 0x1000 + 19 + struct.unpack('<i', bytes.fromhex('a3feffff'))[0],
                'exit_rip': 0x1000 + 19,
                'candidate': str(candidate),
                'expected_code_sha256': hashlib.sha256(code).hexdigest(),
                'tail': [{'rva': rva, 'code': raw, 'mnemonic': mnemonic}
                         for mnemonic, rva, raw in tail],
            }
            contract = generate(model, append, Path(temp) / 'fixture', packet_preflight=preflight)
            self.assertEqual(contract['packet_scope']['terminal_guard'],
                             'removed_by_checked_counted_tail_proof')
            self.assertNotIn(6, contract['reachable_nodes'])
            (candidate / 'expected-code.bin').write_bytes(code + b'\x00')
            with self.assertRaisesRegex(Unsupported, 'expected-code identity'):
                generate(model, append, Path(temp) / 'changed-code', packet_preflight=preflight)
            (candidate / 'expected-code.bin').write_bytes(code)
            preflight['loop_bound_proof']['exit_value'] = 9
            with self.assertRaisesRegex(Unsupported, 'counted tail proof'):
                generate(model, append, Path(temp) / 'bad', packet_preflight=preflight)
        preflight.pop('loop_bound_proof')
        preflight['max_count'] = 2
        preflight['loop_bound_proof'] = {'count': 2}
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(Unsupported, 'source ranges'):
                generate(model, append, Path(temp) / 'unbounded-sources', packet_preflight=preflight)
        preflight.pop('loop_bound_proof')
        preflight['max_count'] = 1
        preflight['capacity'] = 0x1000
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(Unsupported, 'preflight'):
                generate(model, append, Path(temp) / 'fixture', packet_preflight=preflight)

    def test_real_holdout_scratch_matches_independent_postread(self):
        capture = BASE/'cpu-composite-filter-holdout-20260927/candidate-01'
        callee = BASE/'cpu-chain-direct-callee-20260927/candidate-00'
        require_capture(self,capture);require_capture(self,callee)
        from cpu_composite_ir import analyze_paths
        from cpu_append_contract import compose_append_trace, derive_append_contract
        models, _ = analyze_paths(capture)
        append = derive_append_contract(callee)
        packet = compose_append_trace(capture, append)
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / 'holdout'
            contract = generate(models, append, folder)
            self.assertEqual(contract['count'], 32)
            self.assertFalse(contract['full_batch_replacement_allowed'])
            expected = (folder / 'expected.bin').read_bytes()
            stride = contract['output_words_per_call'] * 4
            self.assertEqual(len(packet['records']), 32)
            for row in packet['records']:
                self.assertTrue(row['byte_verified'])
                at = row['iteration_index'] * stride + 4
                self.assertEqual(expected[at:at + 16], bytes.fromhex(row['bytes_hex']))
            compacted = compaction_reference(expected, 32,
                                             contract['output_words_per_call'],
                                             contract['record_stride'] // 4,
                                             packet['initial_cursor_observed'])
            self.assertEqual(compacted['emitted_count'], packet['append_count'])
            self.assertEqual(compacted['final_cursor'], packet['final_cursor_expected'])
            self.assertEqual(compacted['invalid_count'], 0)
            self.assertTrue(compacted['publication_allowed'])

    def test_real_covered_bulk_static_plan_is_not_an_executable_fixture(self):
        capture = BASE/'cpu-composite-bulk-covered-v2-20260927/candidate-01'
        callee = BASE/'cpu-chain-direct-callee-20260927/candidate-00'
        proof = ARTIFACT_BASE/'covered-v2-proof.json'
        for path in (capture,callee):require_capture(self,path)
        require_artifact(self,proof)
        from cpu_composite_ir import analyze_paths
        from cpu_append_contract import derive_append_contract
        models, _ = analyze_paths(capture)
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / 'plan'
            contract = generate_packet_shader_plan(models, derive_append_contract(callee),
                                                   json.loads(proof.read_text()), folder)
            self.assertEqual((contract['count'], contract['input_words_per_call']), (65344, 31))
            self.assertEqual(contract['status'], 'static_shader_plan_binding_incomplete')
            self.assertFalse(contract['executable_gpu_trial_allowed'])
            self.assertFalse((folder / 'input.bin').exists())
            self.assertFalse((folder / 'expected.bin').exists())

    def test_real_covered_bulk_binding_checks_full_artifacts(self):
        capture = BASE/'cpu-composite-bulk-covered-v2-20260927/candidate-01'
        callee = BASE/'cpu-chain-direct-callee-20260927/candidate-00'
        proof_path = ARTIFACT_BASE/'covered-v2-final-proof.json'
        for path in (capture,callee):require_capture(self,path)
        require_artifact(self,proof_path)
        from cpu_composite_ir import analyze_paths
        from cpu_append_contract import derive_append_contract
        proof = json.loads(proof_path.read_text())
        models, _ = analyze_paths(capture)
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / 'bound'
            contract = generate_bulk_fixture(models, derive_append_contract(callee), proof,
                                             proof['artifact_binding']['packed_input']['path'],
                                             proof['artifact_binding']['expected_records']['path'],
                                             folder)
            self.assertEqual((contract['count'], contract['input_words_per_call']), (65344, 31))
            self.assertFalse(contract['full_batch_replacement_allowed'])
            self.assertEqual(contract['packet_scope']['terminal_guard'],
                             'removed_by_checked_counted_tail_proof')
            self.assertEqual(contract['independent_record_sha256'],
                             proof['independent_after_view']['record_sha256'])

    def test_generic_gpu_prefix_scatter_contract_and_mixed_oracle(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / 'compaction'
            contract = generate_compaction(folder)
            self.assertFalse(contract['cpu_prefix_required'])
            self.assertEqual(contract['dispatches'], ['prefix-local', 'prefix-groups', 'scatter'])
            for filename, digest in contract['shader_sha256'].items():
                self.assertEqual(hashlib.sha256((folder / filename).read_bytes()).hexdigest(), digest)
        emitted = {0, 2, 255, 257}
        scratch = b''.join(struct.pack('<IIQ', 0 if i == 1 else 1, i,
                                        4 if i in emitted else 8 if i == 3 else 0)
                           for i in range(258))
        result = compaction_reference(scratch, 258, 4, 1, 0x1000)
        self.assertEqual(result['emitted_count'], 4)
        self.assertEqual(result['invalid_count'], 2)
        self.assertFalse(result['publication_allowed'])
        self.assertEqual(result['slots'][0], 0)
        self.assertEqual(result['slots'][1], None)
        self.assertEqual(result['slots'][255], 2)
        self.assertEqual(result['slots'][257], 3)
        self.assertEqual(result['packed_records'], struct.pack('<IIII', 0, 2, 255, 257))
        self.assertEqual(result['final_cursor'], 0x1010)
        self.assertEqual(result['metadata'], struct.pack('<IQI', 4, 0x1010, 2))
        clean = compaction_reference(struct.pack('<IIQ', 1, 7, 0) +
                                     struct.pack('<IIQ', 1, 8, 4), 2, 4, 1, 0x1000)
        self.assertEqual((clean['emitted_count'], clean['invalid_count'],
                          clean['publication_allowed']), (1, 0, True))


if __name__ == '__main__':
    unittest.main()
