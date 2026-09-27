"""Small machine-code fixtures for bounded control flow and capture order."""
import json
import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from cpu_chain_cfg import Budgets, CodeImage, analyze_capture, analyze_graph


class GraphTests(unittest.TestCase):
    def test_branch_worklist_reaches_code_after_early_ret(self):
        # je +2; ret; nop; mov [rcx],eax; ret
        code = bytes.fromhex('7402c3908901c3')
        graph = analyze_graph(CodeImage(0x1000, [(0x1000, code, 'fixture')]), [0x1000])
        self.assertIn(4, [n['rva'] for n in graph['nodes']])
        self.assertIn('write', [m['kind'] for n in graph['nodes'] for m in n['memory']])
        self.assertIn('branch_taken', [e['kind'] for e in graph['edges']])
        self.assertIn(4, graph['functions'][0]['node_rvas'])
        self.assertEqual(next(n for n in graph['nodes'] if n['rva'] == 4)['function_entries'], [0])

    def test_direct_call_recurses_and_external_call_is_explicit(self):
        # call +5 to 0x100a; ret; padding; mov eax,[rcx]; ret
        code = bytes.fromhex('e805000000c3909090908b01c3')
        graph = analyze_graph(CodeImage(0x1000, [(0x1000, code, 'fixture')]), [0x1000])
        self.assertEqual([f['entry_rva'] for f in graph['functions']], [0, 10])
        self.assertIn('read', [m['kind'] for n in graph['nodes'] for m in n['memory']])
        bounded = analyze_graph(CodeImage(0x1000, [(0x1000, code, 'fixture')]),
                                [0x1000], Budgets(max_depth=0))
        self.assertIn('call_depth_budget', [x['kind'] for x in bounded['limitations']])
        external = analyze_graph(CodeImage(0x1000, [(0x1000, bytes.fromhex('e8ffff0000c3'), 'fixture')]), [0x1000])
        self.assertIn('external_or_uncaptured_call_effects_unknown',
                      [x['kind'] for x in external['limitations']])
        self.assertEqual(external['limitations'][0]['function_entry_rva'], 0)

    def test_indirect_branch_is_explicit(self):
        graph = analyze_graph(CodeImage(0x1000, [(0x1000, bytes.fromhex('ffe0'), 'fixture')]), [0x1000])
        self.assertIn('indirect_branch_unknown', [x['kind'] for x in graph['limitations']])

    def test_atomic_path_is_limited_without_rejecting_other_paths(self):
        # lock add [rcx],eax; ret
        graph = analyze_graph(CodeImage(0x1000, [(0x1000, bytes.fromhex('f00101c3'), 'fixture')]), [0x1000])
        self.assertIn('unsupported_atomic_path', [x['kind'] for x in graph['limitations']])
        self.assertIn(3, [n['rva'] for n in graph['nodes']])

    def test_selection_image_hash_and_captured_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            image = root / 'module-image.bin'
            blob = bytearray(0x400)
            blob[:2] = b'MZ'
            struct.pack_into('<I', blob, 0x3c, 0x80)
            blob[0x80:0x84] = b'PE\0\0'
            struct.pack_into('<H', blob, 0x86, 1)
            struct.pack_into('<H', blob, 0x94, 0xf0)
            struct.pack_into('<H', blob, 0x98, 0x20b)
            struct.pack_into('<Q', blob, 0x98 + 24, 0x140000000)
            sec = 0x80 + 24 + 0xf0
            blob[sec:sec + 5] = b'.text'
            struct.pack_into('<IIII', blob, sec + 8, 16, 0x1000, 16, 0x200)
            struct.pack_into('<I', blob, sec + 36, 0x60000020)
            blob[0x200:0x210] = bytes.fromhex('c3') + b'\x90' * 15
            image.write_bytes(blob)
            selection = root / 'selection.json'
            document = {'image_sha256': hashlib.sha256(blob).hexdigest(),
                        'candidates': [{'entry_rva': 0x1000}]}
            selection.write_text(json.dumps(document))
            result = analyze_capture(root, image=image, selection=selection)
            self.assertEqual(result['seed_rvas'], [0x1000])
            self.assertEqual(result['image_identity']['verification'], 'sha256')
            document['image_sha256'] = '0' * 64
            selection.write_text(json.dumps(document))
            with self.assertRaisesRegex(ValueError, 'SHA'):
                analyze_capture(root, image=image, selection=selection)
            document['image_sha256'] = hashlib.sha256(blob).hexdigest()
            selection.write_text(json.dumps(document))
            watch = root / 'watch'
            watch.mkdir()
            event = dict(event=0, module_base=0x140000000, function_begin=0x140001000)
            (watch / 'writes.jsonl').write_text(json.dumps(event) + '\n')
            (watch / 'function-0.bin').write_bytes(b'\xcc')
            with self.assertRaisesRegex(ValueError, 'captured code differs'):
                analyze_capture(root, image=image, selection=selection)

    def test_capture_discovery_and_ordered_multiwriter(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            watch = root / 'watch'
            watch.mkdir()
            # Two observed stores in one captured function.
            code = bytes.fromhex('89018941fcc3')
            events = [dict(event=i, module_base=0x1000, function_begin=0x1000,
                           rip_after=0x1002 if i == 0 else 0x1005, target=0x2000,
                           bytes=4, qpc=10 + i, tid=7, read_ok=True,
                           value='01000000' if i == 0 else '02000000') for i in range(2)]
            (watch / 'writes.jsonl').write_text(''.join(json.dumps(e) + '\n' for e in events))
            for i in range(2):
                (watch / ('function-%d.bin' % i)).write_bytes(code)
            result = analyze_capture(root)
            self.assertEqual(result['seed_rvas'], [0])
            self.assertEqual(result['writers'][0]['order_status'], 'observed_single_thread_order')
            self.assertEqual([e['observed_role'] for e in result['writers'][0]['events']],
                             ['observed_write', 'update_candidate'])
            self.assertFalse(result['replacement_allowed'])
            events[1]['tid'] = 8
            (watch / 'writes.jsonl').write_text(''.join(json.dumps(e) + '\n' for e in events))
            self.assertEqual(analyze_capture(root)['writers'][0]['order_status'], 'ambiguous')

    def test_only_immediate_store_is_initialization_candidate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            watch = root / 'watch'
            watch.mkdir()
            # mov dword ptr [rcx], 0x12; ret
            code = bytes.fromhex('c70112000000c3')
            event = dict(event=0, module_base=0x1000, function_begin=0x1000,
                         rip_after=0x1006, target=0x2000, bytes=4, qpc=10,
                         tid=7, read_ok=True, value='12000000')
            (watch / 'writes.jsonl').write_text(json.dumps(event) + '\n')
            (watch / 'function-0.bin').write_bytes(code)
            result = analyze_capture(root)
            self.assertEqual(result['writers'][0]['events'][0]['observed_role'],
                             'initialization_candidate')


if __name__ == '__main__':
    unittest.main()
