import importlib.util,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('discovery',Path(__file__).resolve().parents[1]/'scripts/discover-cpu-tasks.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class DecodeTests(unittest.TestCase):
 def loop(self,code):
  ins=list(m.md.disasm(bytes.fromhex(code),0x1000));return m.describe_loop(ins,ins[0].address,ins[-1].address)
 def test_independent_integer_shape_still_requires_memory_contract(self):
  # mov eax,[rsi+rcx*4]; add eax,1; mov [rdi+rcx*4],eax; inc rcx; cmp rcx,rdx; jb
  r=self.loop('8b048e83c00189048f48ffc14839d172ee');self.assertEqual(r['classification'],'affine_map_shape');self.assertFalse(r['admitted']);self.assertIn('cross_iteration_aliasing_unproven',r['reasons']);self.assertEqual([x['iteration_stride_candidate'] for x in r['memory_operands']],[4,4])
 def test_missing_address_register_is_supported(self):
  self.assertIsNone(m.canonical(None))
 def test_call_is_not_pure(self):
  r=self.loop('e80000000048ffc175f6');self.assertIn('calls_have_unclosed_effects_and_dependencies',r['reasons']);self.assertFalse(r['admitted'])
 def test_atomic_rejected(self):
  r=self.loop('f083070148ffc175f7');self.assertIn('unsupported_atomic_or_system_effects',r['reasons']);self.assertFalse(r['admitted'])
 def test_vex_store_is_a_write_not_a_read(self):
  r=self.loop('c5f81002c5f97f064883c2104883c610493bd572eb');stores=[a for a in r['memory_operands'] if a['base']=='rsi'];self.assertTrue(stores[0]['write']);self.assertFalse(stores[0]['read'])
 def test_generic_record_exchange_recognized_without_names(self):
  r=self.loop('c5f81002c5f8100ec5f97f06c5f97f0a');self.assertEqual(r['record_exchanges'][0]['bytes'],16);self.assertFalse(r['admitted'])
 def test_same_output_address_is_not_independent(self):
  r=self.loop('8b048e890748ffc14839d172f3');self.assertIn('output_overlap_between_iterations',r['reasons']);self.assertEqual(r['classification'],'unsupported_or_unclosed_loop')
 def test_vector_reduction_is_not_independent_map(self):
  r=self.loop('0f58064883c61048ffc975f3');self.assertIn('xmm0',r['loop_carried_register_candidates']);self.assertIn('simd_loop_carried_state_requires_recurrence_semantics',r['reasons'])
 def test_pointer_chase_not_affine(self):
  r=self.loop('488b364885f675f8');self.assertIn('memory_address_recurrence_not_proven_affine',r['reasons'])
if __name__=='__main__':unittest.main()
