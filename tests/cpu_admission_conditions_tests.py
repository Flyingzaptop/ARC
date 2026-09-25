import sys,unittest
from pathlib import Path
from dataclasses import replace
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from cpu_admission_conditions import Semantics,Access,before_dispatch,before_commit

class AdmissionTests(unittest.TestCase):
    def setUp(self):
        # Synthetic trusted-provider facts exercise the checker, not game admission.
        self.s=Semantics(True,True,True,True,True)
        self.a=Access(7,2,0x10000,1045504,True,True,True,True,'enforced_complete_access_lease',9)
    def test_semantics_does_not_imply_ownership(self):
        r,t=before_dispatch(self.s,Access());self.assertTrue(r['semantics_pass']);self.assertFalse(r['memory_pass']);self.assertIsNone(t)
    def test_ownership_does_not_imply_semantics(self):
        r,t=before_dispatch(Semantics(),self.a);self.assertTrue(r['memory_pass']);self.assertFalse(r['semantics_pass']);self.assertIsNone(t)
    def test_complete_lease_is_held_until_verified_commit(self):
        _,t=before_dispatch(self.s,self.a)
        self.assertTrue(before_commit(self.s,self.a,t,gpu_complete=True,scratch_verified=True)['eligible'])
    def test_reused_same_address_invalidates_ticket(self):
        _,t=before_dispatch(self.s,self.a)
        for a in (replace(self.a,generation=3),replace(self.a,epoch=10),replace(self.a,allocation_id=8)):
            self.assertFalse(before_commit(self.s,a,t,gpu_complete=True,scratch_verified=True)['eligible'])
    def test_revocation_or_early_publication_blocks_commit(self):
        _,t=before_dispatch(self.s,self.a)
        for field in ('complete_access_domain','exclusive_through_commit','lifetime_pinned','consumer_publication_held'):
            a=replace(self.a,**{field:False})
            self.assertFalse(before_commit(self.s,a,t,gpu_complete=True,scratch_verified=True)['eligible'])
    def test_pending_or_unverified_result_blocks_commit(self):
        _,t=before_dispatch(self.s,self.a)
        for g,v in [(False,True),(True,False)]:
            self.assertFalse(before_commit(self.s,self.a,t,gpu_complete=g,scratch_verified=v)['eligible'])
    def test_observations_are_not_a_provider(self):
        r,_=before_dispatch(self.s,replace(self.a,provider='complete_trace_no_conflicts'))
        self.assertFalse(r['memory_pass'])
    def test_code_or_exit_semantics_cannot_be_relaxed_by_memory(self):
        for field in self.s.__dataclass_fields__:
            r,_=before_dispatch(replace(self.s,**{field:False}),self.a);self.assertFalse(r['eligible'])
    def test_range_wrap_and_no_ticket(self):
        r,_=before_dispatch(self.s,replace(self.a,begin=(1<<64)-16));self.assertFalse(r['eligible'])
        self.assertFalse(before_commit(self.s,self.a,None,gpu_complete=True,scratch_verified=True)['eligible'])
if __name__=='__main__':unittest.main()
