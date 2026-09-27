import copy
import json
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from cpu_append_contract import (
    compose_append_events, compose_append_trace, derive_append_contract,
    derive_shape, preflight_no_growth, preflight_packet_no_growth,
)


EVIDENCE_OVERRIDE = os.environ.get('ARC_COMPOSITE_EVIDENCE_ROOT')
BASE = (Path(EVIDENCE_OVERRIDE) if EVIDENCE_OVERRIDE else
        Path(r'C:\Users\r3d_flzp\ARC-Hardening-GPU\universal-optimizer'))
CAPTURE = BASE / 'cpu-chain-direct-callee-20260927' / 'candidate-00'


def require_capture(case, path):
    if path.exists():
        return
    message = f'missing {path}; set ARC_COMPOSITE_EVIDENCE_ROOT to the extracted capture root'
    if EVIDENCE_OVERRIDE:
        case.fail(message)
    case.skipTest(message)


class AppendContractTests(unittest.TestCase):
    def test_captured_fast_path_is_one_sample_without_live_admission(self):
        require_capture(self, CAPTURE)
        contract = derive_append_contract(CAPTURE)
        self.assertEqual(contract['record_stride'], 16)
        self.assertEqual(contract['container']['cursor_offset'], 8)
        self.assertEqual(contract['container']['capacity_offset'], 16)
        self.assertEqual(contract['sample']['record_count'], 1)
        self.assertFalse(contract['replacement_admitted'])
        self.assertEqual(
            [(w['offset'], w['width']) for w in contract['ordered_writes']
             if 'offset' in w],
            [(0, 16), (12, 4), (12, 4), (10, 2), (12, 1),
             (0, 4), (4, 4), (8, 2)],
        )

    def test_offsets_and_stride_come_from_code_bytes(self):
        require_capture(self, CAPTURE)
        records = copy.deepcopy(json.loads((CAPTURE / 'memory-plan.json').read_text())['records'])
        changes = {
            0x1c2da6: ('488b7118', 24),
            0x1c2dad: ('488b4928', 40),
            0x1c2dca: ('c5fc1106', 0),
            0x1c2dce: ('488b5f18', 24),
            0x1c2dd2: ('488d4320', 0),
            0x1c2dd6: ('48894718', 24),
        }
        for record in records:
            if record['rva'] in changes:
                record['code'] = changes[record['rva']][0]
                record['length'] = len(bytes.fromhex(record['code']))
                if changes[record['rva']][1]:
                    record['operands'][0]['disp'] = changes[record['rva']][1]
        shape = derive_shape(records, 0x1c2d90, 0x1c2f10)
        self.assertEqual((shape['cursor_offset'], shape['capacity_offset'],
                          shape['record_stride']), (24, 40, 32))

    def test_isolated_preflight_bounds_alias_and_overflow(self):
        contract = {'record_stride': 32}
        base = dict(begin=1000, cursor=1064, capacity=1192, count=3,
                    snapshot_bounds=(900, 1300), header_range=(920, 944),
                    source_ranges=((940, 960),))
        self.assertTrue(preflight_no_growth(contract, **base)['input_guards_pass'])
        self.assertFalse(preflight_no_growth(contract, **base)['replacement_admitted'])
        for change, expected in [
            ({'count': 5}, 'would_take_growth_branch'),
            ({'cursor': 1065}, 'misaligned_cursor_or_capacity'),
            ({'snapshot_bounds': (900, 1150)}, 'outside_snapshot_or_overflow'),
            ({'header_range': (1100, 1110)}, 'header_aliases_output'),
            ({'source_ranges': ((1120, 1140),)}, 'source_aliases_output'),
            ({'header_range': None}, 'header_range_not_provided'),
        ]:
            result = preflight_no_growth(contract, **(base | change))
            self.assertFalse(result['input_guards_pass'])
            self.assertEqual(result['reason'], expected)
        overflow = base | {'begin': (1 << 64) - 96,
                           'cursor': (1 << 64) - 64,
                           'capacity': (1 << 64) - 1,
                           'snapshot_bounds': (0, (1 << 64) - 1)}
        self.assertFalse(preflight_no_growth(contract, **overflow)['input_guards_pass'])

    def test_holdout_postmemory_provides_exact_records_and_cursor(self):
        holdout = BASE / 'cpu-composite-filter-holdout-20260927' / 'candidate-01'
        require_capture(self, CAPTURE)
        require_capture(self, holdout)
        contract = derive_append_contract(CAPTURE)
        result = compose_append_trace(holdout, contract)
        self.assertEqual((result['iteration_count'], result['append_count']), (32, 32))
        self.assertEqual(result['emit_flags'], [1] * 32)
        self.assertTrue(result['all_record_bytes_verified'])
        self.assertEqual(result['records'][0]['bytes_hex'],
                         '00000000c2ae000096450100ff410000')
        self.assertEqual(result['final_cursor_observed'] -
                         result['initial_cursor_observed'], 32 * 16)
        self.assertFalse(result['replacement_admitted'])

    def test_earlier_packet_capture_does_not_claim_exact_bytes(self):
        positive = BASE / 'cpu-composite-filter-positive-20260927' / 'candidate-01'
        require_capture(self, CAPTURE)
        require_capture(self, positive)
        result = compose_append_trace(positive, derive_append_contract(CAPTURE))
        self.assertEqual((result['iteration_count'], result['append_count']), (32, 32))
        self.assertFalse(result['all_record_bytes_verified'])
        self.assertIsNone(result['records'][0]['bytes_hex'])
        self.assertIsNone(result['final_cursor_observed'])
        self.assertEqual(result['final_cursor_expected'] -
                         result['initial_cursor_observed'], 32 * 16)

    def test_packet_composition_reject_and_stable_output_slots(self):
        base = 0x100000
        owner = 500
        contract = {
            'entry_rva': 100, 'record_stride': 4,
            'machine_path': {
                'cursor_offset': 8, 'capacity_offset': 16,
                'cursor_load_rva': 101, 'capacity_load_rva': 102,
                'growth_branch_rva': 103, 'growth_target_rva': 200,
                'zero_store_rva': 105,
            },
            'ordered_writes': [
                {'rva': 105, 'offset': 0, 'width': 4},
                {'rva': 107, 'target': 'cursor_header', 'width': 8},
                {'rva': 109, 'offset': 0, 'width': 2},
            ],
        }
        def mem(address, data):
            return {'address': address, 'size': len(data), 'ok': True,
                    'bytes': data.hex()}
        def ev(rva, *, kind=0, memory=(), after=()):
            return {'kind': kind, 'rip': base + rva,
                    'registers': [0, owner] + [0] * 14,
                    'memory': list(memory), 'previous_after': list(after)}
        def append(old, pair):
            return [
                ev(100), ev(101, memory=[mem(owner + 8, old.to_bytes(8, 'little'))]),
                ev(102, memory=[mem(owner + 16, (1020).to_bytes(8, 'little'))]),
                ev(103), ev(104),
                ev(105, memory=[mem(old, b'xxxx')]),
                ev(106, after=[mem(old, b'\0' * 4)]),
                ev(107, memory=[mem(owner + 8, old.to_bytes(8, 'little'))]),
                ev(108, after=[mem(owner + 8, (old + 4).to_bytes(8, 'little'))]),
                ev(109, memory=[mem(old, b'\0\0')]),
                ev(110, after=[mem(old, pair)]),
            ]
        trace = [ev(1, kind=1), *append(1000, b'\x0a\x0b'),
                 ev(2, kind=2), ev(1, kind=1), ev(50), ev(2, kind=2),
                 ev(1, kind=1), *append(1004, b'\x0c\x0d'), ev(2, kind=2)]
        result = compose_append_events(trace, base, contract, initial_cursor=1000)
        self.assertEqual(result['emit_flags'], [1, 0, 1])
        self.assertEqual([r['output_slot'] for r in result['records']], [0, 1])
        self.assertEqual([r['bytes_hex'] for r in result['records']],
                         ['0a0b0000', '0c0d0000'])
        self.assertEqual(result['final_cursor_observed'], 1008)
        empty = compose_append_events([ev(1, kind=1), ev(50), ev(2, kind=2)],
                                      base, contract, initial_cursor=1000)
        self.assertEqual((empty['append_count'], empty['final_cursor_expected']),
                         (0, 1000))
        self.assertIsNone(empty['final_cursor_observed'])
        preflight = preflight_packet_no_growth(
            contract, packet_count=3, begin=1000, cursor=1000,
            capacity=1008, snapshot_bounds=(900, 1100), header_range=(500, 524))
        self.assertFalse(preflight['input_guards_pass'])
        self.assertEqual(preflight['reason'], 'would_take_growth_branch')
        broken = copy.deepcopy(trace)
        second_load = next(e for e in broken[12:] if e['rip'] == base + 101)
        second_load['memory'][0]['bytes'] = (1012).to_bytes(8, 'little').hex()
        with self.assertRaisesRegex(ValueError, 'cursor_chain_discontinuity'):
            compose_append_events(broken, base, contract, initial_cursor=1000)


if __name__ == '__main__':
    unittest.main()
