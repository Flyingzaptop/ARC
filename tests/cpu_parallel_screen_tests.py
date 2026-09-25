import sys,unittest,importlib.util,copy
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from cpu_parallel_screen import assess,incorporate_measurements,ordering,recommendations,context_key,record_attempt
spec=importlib.util.spec_from_file_location('discovery',Path(__file__).resolve().parents[1]/'scripts/discover-cpu-tasks.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class ParallelScreenTests(unittest.TestCase):
    def candidate(self,code='8b048e83c00189048f48ffc14839d172ee',identity='map'):
        ins=list(m.md.disasm(bytes.fromhex(code),0x1000));c=m.describe_loop(ins,ins[0].address,ins[-1].address)
        return dict(c,id=identity,function_rva=0x1000,loop_rva=0x1000,backedge_rva=ins[-1].address,samples=3)
    def measurement(self,**kw):
        return dict(provenance='runtime_measured',evidence='fixture-only',cpu_time_scope='isolated_useful_running_time',cpu_useful_ms=4.,calls_per_frame=2.,batch_elements=1024,input_bytes=4096,output_bytes=4096,upload_ms=.1,return_ms=.1,wait_ms=.1,gpu_ms=.5,dispatch_ms=.1,replacement_total_ms=.9,replacement_time_scope='input_ready_to_all_consumers_ready',**kw)
    def test_map_above_hot_sequential_exchange(self):
        a=assess(self.candidate());c=self.candidate(identity='exchange');c['record_exchanges']=[{'bytes':16}];c['samples']=1000;b=assess(c)
        incorporate_measurements([a,b],{});self.assertLess(ordering(a),ordering(b));self.assertIsNone(b['gpu_algorithm'])
    def test_unknown_allows_bounded_research_not_replacement(self):
        c=assess(self.candidate());incorporate_measurements([c],{})
        self.assertIsNone(c['useful_cpu_ms']);self.assertIsNone(c['exchange']['return_ms']);self.assertTrue(c['bounded_research_eligible']);self.assertFalse(c['replacement_economics_passed'])
    def test_transfer_can_overrule_good_topology(self):
        c=assess(self.candidate());v=self.measurement();v['return_ms']=10;v['replacement_total_ms']=10.8
        incorporate_measurements([c],{'map':v});self.assertLess(c['net_task_ms_estimate'],0);self.assertFalse(c['detailed_capture_eligible'])
    def test_positive_task_can_qualify_for_investigation(self):
        c=assess(self.candidate());incorporate_measurements([c],{'map':self.measurement()});self.assertTrue(c['detailed_capture_eligible'])
    def test_parent_wall_is_not_useful_cpu(self):
        c=assess(self.candidate());v=self.measurement();v['cpu_time_scope']='parent_wall';incorporate_measurements([c],{'map':v});self.assertFalse(c['replacement_economics_passed']);self.assertTrue(c['bounded_research_eligible'])
    def test_manual_override_does_not_drive_auto_rank(self):
        c=assess(self.candidate());v=self.measurement();v['provenance']='manual';incorporate_measurements([c],{'map':v});self.assertTrue(c['manual_facts_used']);self.assertIsNone(c['net_task_ms_estimate'])
    def test_gpu_consumer_requires_edge_evidence(self):
        c=assess(self.candidate());v=self.measurement(gpu_consumer=True);incorporate_measurements([c],{'map':v});self.assertEqual(c['economics_status'],'gpu_residency_unsubstantiated')
    def test_cold_or_single_element_work_not_deep_selected(self):
        for field,value in [('calls_per_frame',0),('batch_elements',1)]:
            c=assess(self.candidate());v=self.measurement();v[field]=value;incorporate_measurements([c],{'map':v});self.assertFalse(c['detailed_capture_eligible'])
    def test_cross_stream_write_conflict_not_independent(self):
        c=self.candidate('8b048e89048f89448f0448ffc14839d172eb')
        a=assess(c);self.assertTrue(a['cross_output_iteration_conflicts']);self.assertNotEqual(a['topology'],'affine_map')
    def test_float_reduction_is_a_separate_hypothesis(self):
        c=assess(self.candidate('0f58064883c61048ffc975f3'))
        self.assertEqual(c['topology'],'reduction_hypothesis');self.assertIn('NaN',c['numeric_requirements'])
    def test_nop_operand_not_a_memory_read(self):
        c=self.candidate('0f1f0048ffc975f8');self.assertFalse(c['memory_operands'])
    def test_backedge_interval_with_external_jump_is_not_parallel_region(self):
        c=self.candidate('0f58064883c61048ffc975f3');c['instructions'].append({'pc':0x1005,'bytes':'e9','mnemonic':'jmp','operands':'0x5000'})
        a=assess(c);self.assertTrue(a['unclosed_control_edges']);self.assertLess(a['feasibility_tier'],2)
    def test_integer_accumulator_is_not_independent_map(self):
        a=assess(self.candidate('03064883c60448ffc975f5'))
        self.assertEqual(a['topology'],'integer_reduction_hypothesis')
    def test_fence_wait_and_gpu_are_not_added_to_total(self):
        c=assess(self.candidate());v=self.measurement();v.update(replacement_total_ms=3.,wait_ms=3.,gpu_ms=2.,upload_ms=.5,return_ms=.5)
        incorporate_measurements([c],{'map':v});self.assertEqual(c['net_task_ms_estimate'],1.);self.assertTrue(c['replacement_economics_passed'])
    def test_components_without_total_do_not_reject(self):
        c=assess(self.candidate());v=self.measurement();del v['replacement_total_ms'];v['wait_ms']=100
        incorporate_measurements([c],{'map':v});self.assertIsNone(c['net_task_ms_estimate']);self.assertTrue(c['bounded_research_eligible'])
    def test_rejected_candidate_skipped_until_material_change(self):
        c=assess(self.candidate());other=assess(self.candidate(identity='next'));v=self.measurement();v['replacement_total_ms']=10
        incorporate_measurements([c,other],{'map':v});ranked=sorted([c,other],key=ordering)
        self.assertEqual(recommendations(ranked)['recommended_cheap_measurement'],'next')
        self.assertIsNone(recommendations([c])['recommended_deep_capture'])
        new=assess(self.candidate());new['sample_hits']=10000;incorporate_measurements([new],{'map':v});self.assertFalse(new['bounded_research_eligible'])
        new=assess(self.candidate());new['context_key']=context_key(new,{'residency':'gpu'});incorporate_measurements([new],{'map':v});self.assertTrue(new['bounded_research_eligible'])
        c['context_key']=context_key(c,{'residency':'gpu'});incorporate_measurements([c],{'map':v})
        self.assertIsNone(c['useful_cpu_ms']);self.assertIsNone(c['exchange']['return_ms']);self.assertTrue(c['bounded_research_eligible'])
    def test_unknown_research_has_attempt_limit(self):
        c=assess(self.candidate());incorporate_measurements([c],{'map':{'provenance':'runtime_measured','research_attempts':3}})
        self.assertFalse(c['bounded_research_eligible']);self.assertIsNone(recommendations([c])['recommended_cheap_measurement'])
    def test_lower_bound_can_reject_but_cannot_approve(self):
        for bound,eligible in [(5,False),(1,True)]:
            c=assess(self.candidate());v=self.measurement();del v['replacement_total_ms'];v.update(replacement_lower_bound_ms=bound,lower_bound_scope='mandatory_input_preparation_for_declared_contract')
            incorporate_measurements([c],{'map':v});self.assertEqual(c['bounded_research_eligible'],eligible);self.assertFalse(c['replacement_economics_passed'])
    def test_attempt_budget_is_persisted(self):
        import tempfile,json
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'measurements.json'
            for _ in range(3):
                c=assess(self.candidate());incorporate_measurements([c],json.loads(path.read_text())['candidates'] if path.exists() else {})
                record_attempt(path,'image',['map'],[c],'capture.json')
            c=assess(self.candidate());incorporate_measurements([c],json.loads(path.read_text())['candidates']);self.assertFalse(c['bounded_research_eligible'])
    def test_execution_condition_reopens_inactive_candidate(self):
        spec=importlib.util.spec_from_file_location('feedback',Path(__file__).resolve().parents[1]/'experiments/record-pack/feed_screen.py');feedback=importlib.util.module_from_spec(spec);spec.loader.exec_module(feedback)
        c=assess(self.candidate());off={'execution_conditions':feedback.execution_conditions({'grid_gate_histogram':{'0':10}})}
        c['context_key']=context_key(c,off);v=self.measurement();v.update(calls_per_frame=0,conditions=off)
        incorporate_measurements([c],{'map':v});self.assertFalse(c['bounded_research_eligible'])
        same=assess(self.candidate());same['context_key']=context_key(same,off);incorporate_measurements([same],{'map':v});self.assertFalse(same['bounded_research_eligible'])
        self.assertEqual(context_key(c,off),context_key(c,{'execution_conditions':feedback.execution_conditions({'grid_gate_histogram':{'0':999}})}))
        c['context_key']=context_key(c,{'execution_conditions':feedback.execution_conditions({'grid_gate_histogram':{'1':10}})})
        incorporate_measurements([c],{'map':v});self.assertTrue(c['bounded_research_eligible']);self.assertIsNone(c['calls_per_frame'])
if __name__=='__main__':unittest.main()
