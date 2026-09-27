import copy
import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from cpu_word_gpu import (Unsupported, generate_plan, generate_compaction,
                          generate_bound_fixture)

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


class WordGpuTests(unittest.TestCase):
    def evidence(self):
        candidate=BASE/'cpu-composite-pack-bulk-20260927/candidate-00'
        group=ARTIFACT_BASE/'grouped-mask-contract-bulk2.json'
        proof=ARTIFACT_BASE/'word-scatter-bulk-proof-v2/proof.json'
        require_capture(self,candidate)
        for path in (group,proof):require_artifact(self,path)
        from cpu_composite_ir import analyze_paths
        models,_=analyze_paths(candidate)
        return models,json.loads(group.read_text()),json.loads(proof.read_text())

    def test_typed_word_plan_excludes_carried_index_and_preserves_guards(self):
        models,group,proof=self.evidence()
        with tempfile.TemporaryDirectory() as temp:
            out=Path(temp)/'word'
            contract=generate_plan(models,group,proof,out)
            self.assertEqual((contract['count'],contract['input_words_per_row'],
                              contract['scratch_words_per_row']), (65343,34,5))
            self.assertEqual(contract['word_source'],models[0]['typed_ir']['outputs'][0]['source'])
            self.assertEqual(contract['guard_count'],len(group['guard_path'])-1)
            self.assertNotIn(159,[item['node'] for item in contract['inputs']])
            self.assertNotIn(118,[item['node'] for item in contract['inputs']])
            self.assertFalse(contract['executable_gpu_trial_allowed'])
            self.assertEqual(contract['shader_sha256'],
                             hashlib.sha256((out/'generated.hlsl').read_bytes()).hexdigest())

    def test_final_metadata_includes_all_five_stack_ranges(self):
        _,_,proof=self.evidence()
        with tempfile.TemporaryDirectory() as temp:
            out=Path(temp)/'compaction'
            contract=generate_compaction(proof,out)
            self.assertEqual((contract['count'],contract['metadata_words']), (65343,9))
            self.assertEqual(len(proof['final_stack_metadata']),5)
            values=struct.unpack('<9I',(out/'metadata-expected.bin').read_bytes())
            self.assertEqual(values[:5],(65343,proof['output_preflight']['exit_index'],
                                         proof['counter_preflight'][0]['after'],
                                         proof['binary_flag_preflight']['after'],0))
            self.assertFalse(contract['replacement_allowed'])
            for name,digest in contract['shader_sha256'].items():
                self.assertEqual(hashlib.sha256((out/name).read_bytes()).hexdigest(),digest)

    def test_unmapped_final_state_rejected(self):
        _,_,proof=self.evidence()
        proof=copy.deepcopy(proof)
        proof['final_stack_metadata'].pop()
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(Unsupported,'final-state recipe'):
                generate_compaction(proof,Path(temp)/'compaction')

    def test_full_snapshot_binding_and_first_two_rows(self):
        models,_,_=self.evidence()
        proof_path=ARTIFACT_BASE/'word-scatter-bulk-proof-v2/final-proof.json'
        require_artifact(self,proof_path)
        proof=json.loads(proof_path.read_text())
        with tempfile.TemporaryDirectory() as temp:
            out=Path(temp)/'bound'
            contract=generate_bound_fixture(models,ROOT/'experiments/cpu-word-gpu/static-plan',
                                             proof,ROOT/'experiments/cpu-word-gpu/compaction',out)
            self.assertEqual((contract['count'],contract['input_words_per_row']), (65343,34))
            self.assertEqual(contract['expected_words_sha256'],proof['expected_words']['sha256'])
            self.assertFalse(contract['live_replacement_allowed'])
            self.assertFalse(contract['homogeneous_guard_class_proven_for_full_count'])
            self.assertEqual(len((out/'metadata-expected.bin').read_bytes()),36)


if __name__=='__main__':unittest.main()
