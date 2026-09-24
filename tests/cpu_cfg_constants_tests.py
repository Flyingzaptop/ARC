import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import capstone
from cpu_cfg_constants import analyze

class MustConstantsTests(unittest.TestCase):
    def analyze(self,code):
        d=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);d.detail=True
        ins=list(d.disasm(bytes.fromhex(code),0));return ins,analyze(ins)
    def test_initializer_must_dominate(self):
        ins,(states,missing)=self.analyze('85c0740548c7c3010000004839d8c3')
        # Use a valid branch to the cmp; malformed targets must remain unknown.
        self.assertTrue(missing)
        ins,(states,missing)=self.analyze('85c0740748c7c3010000004839d8c3')
        self.assertFalse(missing);self.assertNotIn('rbx',states[ins[-2].address])
    def test_path_redefinition_loses_constant(self):
        ins,(states,_)=self.analyze('bb0100000085c07405bb020000004839d8c3')
        self.assertNotIn('rbx',states[ins[-2].address])
    def test_call_destroys_volatile_only_under_abi_assumption(self):
        ins,(states,_)=self.analyze('b801000000bb02000000e8000000004839d8c3')
        self.assertNotIn('rax',states[ins[-2].address])
        self.assertEqual(states[ins[-2].address]['rbx'],2)
    def test_partial_write_is_not_full_constant(self):
        ins,(states,_)=self.analyze('bb01000000b3024839d8c3')
        self.assertNotIn('rbx',states[ins[-2].address])
    def test_loop_redefinition_does_not_leave_stale_constant(self):
        ins,(states,_)=self.analyze('bb010000004839d8bb0200000075f6c3')
        self.assertNotIn('rbx',states[ins[1].address])
    def test_indirect_edge_is_explicit(self):
        _,(_,missing)=self.analyze('ffe0');self.assertEqual(missing,[0])
if __name__=='__main__':unittest.main()
