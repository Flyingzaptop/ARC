import itertools
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from cpu_snapshot_gather import Unsupported, Views, emit_native, finalize_packet_proof, load_manifest, packet_proofs, validate_first_paths

EVIDENCE_OVERRIDE=os.environ.get('ARC_COMPOSITE_EVIDENCE_ROOT')
BASE=Path(EVIDENCE_OVERRIDE) if EVIDENCE_OVERRIDE else Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer')
DATA=BASE/'cpu-composite-bulk-covered-v2-20260927/candidate-01'
COVERED=DATA
OLD_CONTRACT=ROOT/'experiments/cpu-composite-gpu/holdout-32/contract.json'


class SnapshotGatherTests(unittest.TestCase):
    def setUp(self):
        self.manifest=DATA/'native-manifest.json'

    def require(self,path):
        if path.exists():return
        message=f'missing {path}; set ARC_COMPOSITE_EVIDENCE_ROOT to the extracted capture root'
        if EVIDENCE_OVERRIDE:self.fail(message)
        self.skipTest(message)

    def test_first_two_snapshot_leaves(self):
        self.require(self.manifest)
        result=validate_first_paths(DATA,OLD_CONTRACT,self.manifest)
        self.assertEqual(result['checked_leaf_values'],34)
        self.assertEqual(result['packet_count'],65344)

    def test_counted_tail_and_capacity(self):
        self.require(self.manifest)
        result=packet_proofs(DATA,self.manifest)
        proof=result['loop_bound_proof'];pre=result['packet_preflight']
        self.assertEqual(proof['status'],'verified_counted_tail')
        self.assertEqual(proof['entry_value']+proof['count']*proof['step'],proof['bound_value'])
        self.assertEqual([x['mnemonic'] for x in proof['tail']],['add','cmp','jne'])
        self.assertEqual(pre['required_bytes'],65344*16)
        self.assertGreater(pre['headroom_bytes'],0)

    def test_view_bounds_fail_closed(self):
        self.require(self.manifest)
        manifest=load_manifest(self.manifest)
        with Views(manifest) as views:
            self.assertEqual(len(views.read(manifest['cursor_address'],8)),8)
            with self.assertRaisesRegex(Unsupported,'outside owned views'):
                views.read(1,8)

    def test_generated_native_gather_requires_pruned_contract(self):
        self.require(self.manifest)
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp)
            with self.assertRaisesRegex(Unsupported,'per-row cursor'):
                emit_native(DATA,OLD_CONTRACT,self.manifest,folder/'bad')
            contract=json.loads(OLD_CONTRACT.read_text())
            rows=[item for item in contract['inputs'] if item['node'] not in {107,108,110,113}]
            width={'u8':1,'u16':2,'u32':4,'u64':8,'f32':4,'u32x4':16,'f32x4':16}
            spans=[(width[item['type']]+3)//4 for item in rows]
            for item,slot in zip(rows,itertools.accumulate([0]+spans[:-1])):item['slot']=slot
            contract['inputs']=rows;contract['input_words_per_call']=sum(spans)
            path=folder/'contract.json';path.write_text(json.dumps(contract))
            result=emit_native(DATA,path,self.manifest,folder/'generated')
            self.assertEqual(result['row_bytes'],124)
            self.assertEqual((folder/'generated/first-two-expected.bin').stat().st_size,248)
            source=(folder/'generated/gather.cpp').read_text()
            self.assertIn('source aliases output reservation',source)
            self.assertIn('unowned snapshot address',source)
            fast=emit_native(DATA,path,self.manifest,folder/'fast',optimized=True)
            self.assertGreater(fast['optimized_hoisted_nodes'],0)
            fast_source=(folder/'fast/gather.cpp').read_text()
            self.assertIn('specialized source region mismatch',fast_source)
            self.assertIn('source aliases output reservation',fast_source)
            split=emit_native(DATA,path,self.manifest,folder/'split',optimized=True,separate_diagnostics=True)
            self.assertTrue(split['diagnostics_outside_steady_timer'])
            self.assertTrue(split['full_per_batch_prep_includes_verification'])
            split_source=(folder/'split/gather.cpp').read_text()
            self.assertIn('verification_ms=',split_source)
            self.assertIn('full_prep_ms=',split_source)
            self.assertIn('collect_ranges=false;',split_source)

    def test_source_range_preflight(self):
        self.require(self.manifest)
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp);proof=packet_proofs(DATA,self.manifest)
            path=folder/'proof.json';path.write_text(json.dumps(proof))
            manifest=load_manifest(self.manifest)
            base=manifest['regions'][0]['base'];ranges=folder/'ranges.csv'
            ranges.write_text(f'begin,end\n{base},{base+4}\n')
            result=finalize_packet_proof(DATA,path,ranges,DATA/'append-contract.json',folder/'final.json')
            self.assertTrue(result['append_preflight_result']['input_guards_pass'])
            self.assertEqual(result['source_ranges_status'],'bounded_native_gather_observed')
            output=proof['packet_preflight']['cursor'];ranges.write_text(f'begin,end\n{output},{output+4}\n')
            with self.assertRaisesRegex(Unsupported,'source alias'):
                finalize_packet_proof(DATA,path,ranges,DATA/'append-contract.json',folder/'bad.json')

    def test_covered_v2_current_snapshot(self):
        self.require(COVERED/'native-manifest.json')
        validation=validate_first_paths(COVERED,OLD_CONTRACT,COVERED/'native-manifest.json')
        self.assertEqual(validation['checked_leaf_values'],34)
        proof=packet_proofs(COVERED,COVERED/'native-manifest.json')
        self.assertEqual(proof['loop_bound_proof']['count'],65344)
        self.assertEqual(proof['independent_after_view']['record_bytes'],65344*16)


if __name__=='__main__':unittest.main()
