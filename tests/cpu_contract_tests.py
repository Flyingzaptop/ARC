import sys,unittest,importlib.util
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from cpu_state_liveness import Liveness,physical
import capstone
from capstone.x86 import X86_OP_MEM
spec=importlib.util.spec_from_file_location('select_contracts',Path(__file__).resolve().parents[1]/'scripts/select-cpu-contracts.py');s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
d=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);d.detail=True
class ContractTests(unittest.TestCase):
 def test_32bit_write_defines_upper_half(self):
  l=Liveness()
  for i in d.disasm(bytes.fromhex('b8010000004889c1'),0):l.add(i)
  self.assertNotIn('rax',l.result())
 def test_partial_write_keeps_high_half_live(self):
  l=Liveness()
  for i in d.disasm(bytes.fromhex('b0014889c1'),0):l.add(i)
  self.assertEqual(l.result()['rax'],hex(((1<<64)-1)^255))
 def test_zeroing_idiom_does_not_read_input(self):
  l=Liveness()
  for i in d.disasm(bytes.fromhex('31c04889c1'),0):l.add(i)
  self.assertNotIn('rax',l.result())
 def test_pure_plan_never_skips_load_store_or_call(self):
  for code in ['488b07','488907','e800000000','c5f97f06','c3']:
   i=next(d.disasm(bytes.fromhex(code),0));self.assertFalse(s.pure_instruction(i))
 def test_lea_is_address_calculation(self):
  self.assertTrue(s.pure_instruction(next(d.disasm(bytes.fromhex('488d442408'),0))))
if __name__=='__main__':unittest.main()
