import json
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from cpu_ir_gpu import Unsupported, _compile_call, _evaluate, generate
_original_path=ROOT/'experiments/cpu-ir-gpu/original_slice.py'
_spec=importlib.util.spec_from_file_location('original_slice',_original_path)
original_slice=importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(original_slice)


def leaf(address):
    return {'kind':'memory_input','address':address,'bytes':4}


def expression(op,a,b):
    return {'op':op,'precision':'float32','left':[a]*4,'right':[b]*4}


def call(role='holdout'):
    a,b=leaf(11),leaf(22)
    return {'call':2,'role':role,'status':'exact_output_replay',
            'input_memory':[{'address':11,'bytes':struct.pack('<f',2).hex()},
                            {'address':22,'bytes':struct.pack('<f',3).hex()}],
            'output':struct.pack('<f',5).hex(),
            'output_provenance':[{'offset':0,'bytes':4,'expression':expression('vaddss',a,b)}]}


class CodegenTests(unittest.TestCase):
    def test_scalar_graph_and_fixture(self):
        sample=call()
        with tempfile.TemporaryDirectory() as temp:
            out=Path(temp)/'fixture'
            contract=generate({'calls':[sample]},out)
            self.assertEqual(contract['input_words_per_call'],2)
            self.assertEqual((out/'input.bin').read_bytes(),struct.pack('<ff',2,3))
            self.assertEqual((out/'expected.bin').read_bytes(),struct.pack('<f',5))
            self.assertIn('precise float t0 = v0 + v1;', (out/'generated.hlsl').read_text())

    def test_unsupported_leaf_and_precision_fail_closed(self):
        sample=call()
        sample['output_provenance'][0]['expression']['left']=[{'kind':'live_register','name':'xmm0'}]*4
        with self.assertRaisesRegex(Unsupported,'unsupported leaf'):
            _compile_call(sample)
        sample=call()
        sample['output_provenance'][0]['expression']['precision']='float64'
        with self.assertRaisesRegex(Unsupported,'precision'):
            _compile_call(sample)

    def test_output_and_topology_validation(self):
        sample=call()
        sample['output']='00000000'
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(Unsupported,'reference differs'):
                generate({'calls':[sample]},Path(temp)/'fixture')
        a=call('reconstruction_check');b=call('holdout')
        b['output_provenance'][0]['expression']['op']='vmulss'
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(Unsupported,'topology'):
                generate({'calls':[a,b]},Path(temp)/'fixture')

    def test_real_packet_holdout(self):
        packet=Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/cpu-batch-final-20260927')
        if not packet.exists():
            self.skipTest('local captured packet absent')
        replay=json.loads((ROOT/'docs/evidence/cpu-producer-20260927/result.json').read_text())['replay']
        with tempfile.TemporaryDirectory() as temp:
            contract=generate(replay,Path(temp)/'fixture',packet_root=packet)
            self.assertEqual(contract['count'],768)
            self.assertEqual(sum(c['role']=='holdout' for c in contract['calls']),256)
            fixture=Path(temp)/'fixture'
            gather=(fixture/'gather.bin').read_bytes()
            magic=gather[:4]
            version,count,stride,words,shared=struct.unpack_from('<IIIII',gather,4)
            self.assertEqual((magic,version,count,stride,words),(b'AIRG',1,768,32,7))
            slots=struct.unpack_from('<'+'i'*words,gather,24)
            shared_at=24+words*4;rows_at=shared_at+shared*4
            rebuilt=bytearray()
            for i in range(count):
                for slot in slots:
                    pos=(rows_at+i*stride+slot) if slot>=0 else (shared_at+(-1-slot)*4)
                    rebuilt.extend(gather[pos:pos+4])
            self.assertEqual(rebuilt,(fixture/'input.bin').read_bytes())

    def test_original_slice_guard(self):
        packet=Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/cpu-batch-final-20260927')
        if not packet.exists():self.skipTest('local captured packet absent')
        blob,meta=original_slice.build(packet)
        self.assertEqual(meta['original_instruction_count'],19)
        self.assertEqual(len(blob),136)
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for folder,name in [('slice','plan.json'),('slice','trace.jsonl'),('batch','plan.json')]:
                (root/folder).mkdir(exist_ok=True)
                (root/folder/name).write_bytes((packet/folder/name).read_bytes())
            plan=json.loads((root/'slice/plan.json').read_text())
            first=next(n for n in plan['instructions'] if any(m['base']==16 for m in n['memory']))
            first['code']='e900000000'
            (root/'slice/plan.json').write_text(json.dumps(plan))
            with self.assertRaisesRegex(original_slice.UnsafeSlice,'plan/decode mismatch'):
                original_slice.build(root)


if __name__=='__main__':unittest.main()
