import sys,unittest,importlib.util,copy
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from cpu_parallel_screen import assess,incorporate_measurements,ordering
spec=importlib.util.spec_from_file_location('discovery',Path(__file__).resolve().parents[1]/'scripts/discover-cpu-tasks.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class ParallelScreenTests(unittest.TestCase):
    def candidate(self,code='8b048e83c00189048f48ffc14839d172ee',identity='map'):
        ins=list(m.md.disasm(bytes.fromhex(code),0x1000));c=m.describe_loop(ins,ins[0].address,ins[-1].address)
        return dict(c,id=identity,function_rva=0x1000,loop_rva=0x1000,backedge_rva=ins[-1].address,samples=3)
    def measurement(self,**kw):
        return dict(provenance='runtime_measured',evidence='fixture-only',cpu_time_scope='isolated_useful_running_time',cpu_useful_ms=4.,calls_per_frame=2.,batch_elements=1024,input_bytes=4096,output_bytes=4096,upload_ms=.1,return_ms=.1,wait_ms=.1,gpu_ms=.5,dispatch_ms=.1,**kw)
    def test_map_above_hot_sequential_exchange(self):
        a=assess(self.candidate());c=self.candidate(identity='exchange');c['record_exchanges']=[{'bytes':16}];c['samples']=1000;b=assess(c)
        incorporate_measurements([a,b],{});self.assertLess(ordering(a),ordering(b));self.assertIsNone(b['gpu_algorithm'])
    def test_unknown_is_not_zero_or_deep_permission(self):
        c=assess(self.candidate());incorporate_measurements([c],{})
        self.assertIsNone(c['useful_cpu_ms']);self.assertIsNone(c['exchange']['return_ms']);self.assertFalse(c['detailed_capture_eligible'])
    def test_transfer_can_overrule_good_topology(self):
        c=assess(self.candidate());v=self.measurement();v['return_ms']=10
        incorporate_measurements([c],{'map':v});self.assertLess(c['net_task_ms_estimate'],0);self.assertFalse(c['detailed_capture_eligible'])
    def test_positive_task_can_qualify_for_investigation(self):
        c=assess(self.candidate());incorporate_measurements([c],{'map':self.measurement()});self.assertTrue(c['detailed_capture_eligible'])
    def test_parent_wall_is_not_useful_cpu(self):
        c=assess(self.candidate());v=self.measurement();v['cpu_time_scope']='parent_wall';incorporate_measurements([c],{'map':v});self.assertFalse(c['detailed_capture_eligible'])
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
if __name__=='__main__':unittest.main()
