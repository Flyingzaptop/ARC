import json
import hashlib
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from cpu_word_snapshot import Unsupported as WordUnsupported, finalize_gather, gather_manifest, packet_proofs
from cpu_snapshot_gather import Unsupported, emit_native

EVIDENCE_OVERRIDE=os.environ.get('ARC_COMPOSITE_EVIDENCE_ROOT')
BASE=Path(EVIDENCE_OVERRIDE) if EVIDENCE_OVERRIDE else Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer')
DATA=BASE/'cpu-composite-pack-bulk-20260927/candidate-00'
ARTIFACT_OVERRIDE=os.environ.get('ARC_COMPOSITE_ARTIFACT_ROOT')
ARTIFACT_BASE=Path(ARTIFACT_OVERRIDE) if ARTIFACT_OVERRIDE else ROOT/'build/cpu-ir-gpu'


class WordSnapshotTests(unittest.TestCase):
    def setUp(self):
        if not (DATA/'views.json').exists():
            message=f'missing {DATA / "views.json"}; set ARC_COMPOSITE_EVIDENCE_ROOT to the extracted capture root'
            if EVIDENCE_OVERRIDE:self.fail(message)
            self.skipTest(message)

    def test_counted_word_and_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            directory=Path(temp)/'proof';proof=packet_proofs(DATA,directory)
            self.assertEqual(proof['loop_bound_proof']['count'],65343)
            self.assertEqual(proof['expected_words']['bytes'],65343*4)
            self.assertEqual(len(proof['final_stack_metadata']),5)
            kinds={item['recipe']['kind'] for item in proof['final_stack_metadata']}
            self.assertEqual(kinds,{'last_item_snapshot_leaf','binary_initial_or_gpu_conditions','initial_plus_emitted_count'})
            manifest=gather_manifest(DATA,proof,directory/'gather-manifest.json')
            self.assertEqual(manifest['packet_count'],65343)
            self.assertEqual(manifest['induction_register'],'r15')

    def test_carried_leaves_fail_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            directory=Path(temp)/'proof';proof=packet_proofs(DATA,directory)
            manifest_path=directory/'gather-manifest.json';gather_manifest(DATA,proof,manifest_path)
            contract={'inputs':[{'node':159,'slot':0,'type':'u64','origin':'entry_register'}],
                      'input_words_per_call':2}
            contract_path=directory/'bad-contract.json';contract_path.write_text(json.dumps(contract))
            with self.assertRaisesRegex(Unsupported,'carried or terminal register'):
                emit_native(DATA,contract_path,manifest_path,directory/'bad',packet_kind='word_scatter',
                            word_proof_path=directory/'proof.json')

    def test_source_alias_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            directory=Path(temp)/'proof';proof=packet_proofs(DATA,directory)
            contract=json.loads((ROOT/'experiments/cpu-word-gpu/static-plan/contract.json').read_text())
            contract_path=directory/'contract.json';contract_path.write_text(json.dumps(contract))
            packed=directory/'input.bin';packed.write_bytes(bytes(proof['loop_bound_proof']['count']*contract['input_words_per_row']*4))
            (directory/'first-two-expected.bin').write_bytes(bytes(contract['input_words_per_row']*8))
            output=proof['output_preflight']['output_start'];ranges=directory/'ranges.csv'
            ranges.write_text(f'begin,end\n{output},{output+4}\n')
            with self.assertRaisesRegex(WordUnsupported,'aliases output'):
                finalize_gather(DATA,directory/'proof.json',contract_path,packed,ranges,directory/'bad.json')

    def test_bound_full_gather_artifacts(self):
        proof=ARTIFACT_BASE/'word-scatter-bulk-proof-v2/final-proof.json'
        if not proof.exists():
            if ARTIFACT_OVERRIDE:self.fail(f'missing {proof} under ARC_COMPOSITE_ARTIFACT_ROOT')
            self.skipTest('full gather artifact absent; set ARC_COMPOSITE_ARTIFACT_ROOT')
        data=json.loads(proof.read_text())
        self.assertEqual(data['status'],'word_scatter_snapshot_gather_bound')
        self.assertEqual(data['artifact_binding']['packed_input']['bytes'],65343*34*4)
        self.assertEqual(data['source_alias_preflight']['status'],'owned_nonaliasing_before_views')
        self.assertEqual(len(data['final_stack_metadata']),5)
        packed=ARTIFACT_BASE/'word-gather-static/input.bin'
        if packed.exists():
            self.assertEqual(hashlib.sha256(packed.read_bytes()).hexdigest(),
                             data['artifact_binding']['packed_input']['sha256'])
        elif ARTIFACT_OVERRIDE:self.fail(f'missing {packed} under ARC_COMPOSITE_ARTIFACT_ROOT')


if __name__=='__main__':unittest.main()
