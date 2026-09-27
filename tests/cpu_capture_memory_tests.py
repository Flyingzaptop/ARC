import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from cpu_capture_memory import build, checked_functions, UNSUPPORTED


def fixture(directory, functions):
    blob = bytearray(struct.pack('<I', len(functions)))
    for rva, code in functions:
        blob.extend(struct.pack('<II', rva, len(code)))
        blob.extend(code)
    (directory / 'checked-functions.bin').write_bytes(blob)


class CaptureMemoryTests(unittest.TestCase):
    def test_read_store_rip_and_stack(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            # mov eax,[rcx]; mov [rdx],eax; mov eax,[rip-8]; ret
            fixture(directory, [(0x1000, bytes.fromhex('8b0189028b05f8ffffffc3'))])
            plan = build(directory)
            rows = plan['records']
            self.assertEqual([r['rva'] for r in rows], [0x1000, 0x1002, 0x1004, 0x100a])
            self.assertEqual(rows[0]['operands'][0]['read'], True)
            self.assertEqual(rows[0]['operands'][0]['write'], False)
            self.assertEqual(rows[1]['operands'][0]['write'], True)
            self.assertEqual(rows[2]['operands'][0]['base'], 16)
            self.assertEqual(rows[2]['operands'][0]['disp'], -8)
            self.assertEqual(rows[3]['operands'][0]['base'], 4)
            self.assertEqual(rows[3]['operands'][0]['read'], True)
            self.assertEqual(plan['coverage_status'], 'complete_decoded_operand_map')
            data = (directory / 'memory-rvas.bin').read_bytes()
            self.assertEqual(struct.unpack_from('<I', data)[0], 4)
            rva, length, n = struct.unpack_from('<III', data, 4)
            self.assertEqual((rva, length, n), (0x1000, 2, 1))
            self.assertEqual(struct.unpack_from('<iiiqI', data, 16), (1, -1, 1, 0, 4))
            self.assertEqual(struct.unpack_from('<III', data, len(data) - 36),
                             (0x100a, 1, 1))

    def test_known_no_memory_is_distinct_from_unsupported(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            # lea rax,[rcx+4]; mov eax,fs:[rax]; ret
            fixture(directory, [(0x2000, bytes.fromhex('488d4104648b00c3'))])
            rows = build(directory)['records']
            self.assertEqual(rows[0]['status'], 'covered')
            self.assertEqual(rows[0]['operands'], [])
            self.assertEqual(rows[1]['status'], 'unsupported')
            self.assertIn('segment', rows[1]['reason'])
            self.assertEqual(struct.unpack_from('<III', (directory / 'memory-rvas.bin').read_bytes(), 4),
                             (0x2000, 4, 0))

    def test_scalar_simd_movsd_is_an_explicit_read(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            fixture(directory, [(0x4000, bytes.fromhex('f20f1001'))])
            row = build(directory)['records'][0]
            self.assertEqual(row['status'], 'covered')
            self.assertEqual((row['operands'][0]['read'], row['operands'][0]['size']), (True, 8))

    def test_gs_teb_and_ordinary_stack_forms(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            # mov eax,gs:[0x58]; push rax; pop rax; call qword [rax]; ret
            fixture(directory, [(0x5000, bytes.fromhex('658b0425580000005058ff10c3'))])
            rows = build(directory)['records']
            self.assertEqual(rows[0]['operands'][0]['base'], 17)
            self.assertEqual(rows[0]['operands'][0]['disp'], 0x58)
            self.assertEqual(rows[1]['operands'][0]['disp'], -8)
            self.assertTrue(rows[1]['operands'][0]['write'])
            self.assertEqual(rows[2]['operands'][0]['disp'], 0)
            self.assertTrue(rows[2]['operands'][0]['read'])
            self.assertEqual(len(rows[3]['operands']), 2)
            self.assertEqual((rows[3]['operands'][0]['read'], rows[3]['operands'][1]['write']),
                             (True, True))
            self.assertEqual(rows[4]['operands'][0]['base'], 4)

    def test_gs_store_keeps_write_access(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            fixture(directory, [(0x5800, bytes.fromhex('6589042558000000'))])
            operand = build(directory)['records'][0]['operands'][0]
            self.assertEqual(operand['base'], 17)
            self.assertTrue(operand['write'])

    def test_push16_stays_unsupported(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            fixture(directory, [(0x6000, bytes.fromhex('6650'))])
            row = build(directory)['records'][0]
            self.assertEqual(row['status'], 'unsupported')
            self.assertEqual(row['reason'], 'nonstandard_stack_width_or_form')
            self.assertEqual(struct.unpack_from('<III', (directory / 'memory-rvas.bin').read_bytes(), 4),
                             (0x6000, 2, UNSUPPORTED))

    def test_pop_rsp_destination_uses_post_increment_address(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            # pop qword ptr [rsp]; pop qword ptr [rsp+0x10]
            fixture(directory, [(0x7000, bytes.fromhex('8f04248f442410'))])
            rows = build(directory)['records']
            self.assertEqual(len(rows), 2)
            self.assertEqual([op['disp'] for op in rows[0]['operands']], [8, 0])
            self.assertEqual([op['disp'] for op in rows[1]['operands']], [24, 0])
            self.assertTrue(rows[0]['operands'][0]['write'])
            self.assertTrue(rows[0]['operands'][1]['read'])

    def test_pop_address_size_override_is_unsupported(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            fixture(directory, [(0x7100, bytes.fromhex('678f0424'))])
            row = build(directory)['records'][0]
            self.assertEqual(row['reason'], 'pop_address_size_override')
            self.assertEqual(struct.unpack_from('<III', (directory / 'memory-rvas.bin').read_bytes(), 4),
                             (0x7100, 4, UNSUPPORTED))

    def test_decode_gap_and_truncated_input_are_explicit(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            fixture(directory, [(0x3000, bytes.fromhex('c30f'))])
            plan = build(directory)
            self.assertEqual(plan['decode_gaps'][0]['begin_rva'], 0x3001)
            self.assertEqual(plan['coverage_status'], 'missing_or_unsupported')
            (directory / 'checked-functions.bin').write_bytes(struct.pack('<III', 1, 0x3000, 99))
            with self.assertRaisesRegex(ValueError, 'size'):
                checked_functions(directory / 'checked-functions.bin')


if __name__ == '__main__':
    unittest.main()
