import os
import sys
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from cpu_composite_gather import _dependencies, _variation, derive, derive_homogeneous_group_contract, derive_stateful_packet, verify_group_bulk_views

EVIDENCE_OVERRIDE=os.environ.get('ARC_COMPOSITE_EVIDENCE_ROOT')
BASE=Path(EVIDENCE_OVERRIDE) if EVIDENCE_OVERRIDE else Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer')


def require_capture(case,path):
    if path.exists():return
    message=f'missing {path}; set ARC_COMPOSITE_EVIDENCE_ROOT to the extracted capture root'
    if EVIDENCE_OVERRIDE:case.fail(message)
    case.skipTest(message)


class GatherTests(unittest.TestCase):
    def test_variation_is_observation_only(self):
        self.assertEqual(_variation([7,7,7])['pattern'],'same_observed_value')
        self.assertEqual(_variation([3,7,11])['delta'],4)
        self.assertEqual(_variation([1,3,8])['pattern'],'varied_observed_values')

    def test_dependency_closure(self):
        nodes=[{'inputs':[]},{'inputs':[0]},{'inputs':[{'id':1,'byte_offset':2}]}]
        self.assertEqual(_dependencies(nodes,2),{0,1,2})

    def test_positive_holdout_recipe(self):
        path=BASE/'cpu-composite-filter-holdout-20260927/candidate-01'
        require_capture(self,path)
        result=derive(path)
        self.assertEqual(result['path_count'],32)
        self.assertTrue(result['typed_topology_identical'])
        self.assertEqual(result['iteration_tail_steps'],[{'register':'r12','step':4,'observed_in_tail':True}])
        self.assertEqual(len(result['observed_cross_path_edges']),62)
        self.assertEqual(sum(s['availability']=='loop_carried_after_first_path' for s in result['memory_inputs']),2)
        self.assertTrue(all(s['initial_seed_required'] for s in result['memory_inputs'] if s['availability']=='loop_carried_after_first_path'))
        self.assertEqual(len(result['ordered_external_writes']),9)
        self.assertFalse(result['independent_output_scatter_supported'])
        self.assertTrue(any(w['address_depends_on_loop_carried_cursor'] for w in result['ordered_external_writes']))
        self.assertTrue(any(w['value_depends_on_loop_carried_cursor'] for w in result['ordered_external_writes']))
        self.assertFalse(result['live_gather_allowed'])
        self.assertFalse(result['executable_gather_emitted'])

    def test_positive_train_same_structural_recipe(self):
        path=BASE/'cpu-composite-filter-positive-20260927/candidate-01'
        require_capture(self,path)
        result=derive(path)
        self.assertEqual(result['path_count'],32)
        self.assertEqual(len(result['memory_inputs']),31)
        self.assertEqual(len(result['observed_cross_path_edges']),62)

    def test_grouped_mask_state_is_explicit(self):
        path=BASE/'cpu-chain-iterations-holdout-20260927/candidate-00'
        require_capture(self,path)
        result=derive_stateful_packet(path)
        self.assertEqual(result['path_count'],1)
        self.assertEqual(result['external_writes_total'],1)
        self.assertGreater(len(result['paths'][0]['preexisting_stack_reads']),0)
        self.assertGreater(len(result['paths'][0]['stack_writes']),0)
        self.assertFalse(result['independent_items_proven'])

    def test_homogeneous_group_limited_contract(self):
        path=BASE/'cpu-composite-pack-train-20260927/candidate-00'
        require_capture(self,path)
        result=derive_homogeneous_group_contract(path)
        self.assertEqual(result['status'],'observed_homogeneous_group_class')
        self.assertEqual(result['path_count'],32)
        self.assertEqual(result['word_scatter']['native_postimage_bytes_verified'],128)
        self.assertEqual(result['word_scatter']['word_stride'],4)
        self.assertEqual(len(result['carried_stack_edges']),62)
        self.assertEqual(len(result['counter_candidates']),1)
        self.assertEqual(result['binary_flag_select_proof']['status'],'binary_select_or_structure')
        self.assertEqual(result['binary_flag_select_proof']['runtime_preflight_required'],'initial_flag in {0,1}')
        self.assertEqual(result['capture_hints']['remaining_elements_observed'],16056)
        self.assertFalse(result['group_membership_enforced'])

    def test_grouped_bulk_before_after_views(self):
        path=BASE/'cpu-composite-pack-bulk-20260927/candidate-00'
        require_capture(self,path/'views.json')
        result=verify_group_bulk_views(path)
        self.assertEqual(result['status'],'bulk_group_snapshot_consistent')
        self.assertEqual(result['count'],65343)
        self.assertEqual(result['output_bytes'],65343*4)
        self.assertEqual(result['counters'][0]['delta'],65343)
        self.assertFalse(result['homogeneous_guard_class_proven_for_full_count'])


if __name__=='__main__':unittest.main()
