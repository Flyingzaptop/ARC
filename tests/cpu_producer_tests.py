import copy,hashlib,json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from cpu_producer_replay import replay_call,Unsupported
from analyze_cpu_producer import valid_scope,valid_stop
from cpu_producer_plan import plan
DATA=json.loads((Path(__file__).parent/'fixtures/cpu_producer_replay.json').read_text())
class ProducerTests(unittest.TestCase):
    def test_ambiguous_writer_is_not_silently_selected(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);(p/'writes.jsonl').write_text('\n'.join(json.dumps({'rip_after':pc,'read_ok':True}) for pc in [1,2]))
            with self.assertRaisesRegex(ValueError,'ambiguous'):plan(p,p/'plan')
    def copy_case(self,load):
        first=copy.deepcopy(DATA['events'][0]);first.update(pc=4096,kind=1,call=0,output=12288,output_ok=True,value='00000000',xmm='00'*256,mxcsr=0x1f80);first['registers']=[0]*16;first['registers'][0]=0x44332211;first['registers'][1]=8192;first['registers'][2]=12288
        store=copy.deepcopy(first);store.update(pc=4098 if load else 4096,kind=0 if load else 1,code='8902',memory=[{'address':12288,'size':4,'ok':True,'bytes':'00000000'}]);last=copy.deepcopy(store);last.update(pc=4100 if load else 4098,kind=2,value='11223344',code='',memory=[])
        nodes=[{'pc':store['pc'],'size':2,'code':'8902'}];rows=[store,last]
        if load:first.update(code='8b01',memory=[{'address':8192,'size':4,'ok':True,'bytes':'11223344'}]);rows.insert(0,first);nodes.insert(0,{'pc':4096,'size':2,'code':'8b01'})
        return replay_call({'instructions':nodes,'stop':last['pc'],'output_bytes':4},rows)
    def test_copy_is_not_called_computation(self):self.assertEqual(self.copy_case(True)['classification'],'copy_only')
    def test_register_copy_keeps_unknown_origin(self):self.assertEqual(self.copy_case(False)['classification'],'origin_unresolved_register_value')
    def call(self,n=2):return copy.deepcopy([e for e in DATA['events'] if e['call']==n])
    def test_heldout_exact(self):self.assertEqual(replay_call(DATA['plan'],self.call())['status'],'exact_output_replay')
    def test_initial_xmm_is_not_an_unrecovered_result(self):
        e=self.call();expected=replay_call(DATA['plan'],e)['output'];e[0]['xmm']='00'*256;self.assertEqual(replay_call(DATA['plan'],e)['output'],expected)
    def test_wrong_producer(self):
        e=self.call();e[1]['pc']+=1
        with self.assertRaises(Unsupported):replay_call(DATA['plan'],e)
    def test_changed_machine_code(self):
        e=self.call();e[0]['code']='90'
        with self.assertRaises(Unsupported):replay_call(DATA['plan'],e)
    def test_unclosed_input(self):
        e=self.call();next(x for x in e if x['memory'])['memory']=[]
        with self.assertRaises(Unsupported):replay_call(DATA['plan'],e)
    def test_new_input_output_mismatch(self):
        e=self.call();v=bytearray.fromhex(e[-1]['value']);v[0]^=1;e[-1]['value']=v.hex()
        with self.assertRaises(ValueError):replay_call(DATA['plan'],e)
    def test_rounding_not_silently_relaxed(self):
        e=self.call();e[0]['mxcsr']|=0x2000
        with self.assertRaises(Unsupported):replay_call(DATA['plan'],e)
    def test_stale_binding(self):
        p=b'{"event":"selected","resource":1}\n';b={'session_event_sha256':hashlib.sha256(p).hexdigest(),'resource':1,'generation':2};retired=b'{"event":"invalidate","resource":1,"generation":2,"qpc":20}\n';self.assertFalse(valid_scope(b,p,p+retired,30));self.assertTrue(valid_scope(b,p,p+retired,10))
    def test_session_mismatch(self):
        p=b'{}\n';b={'session_event_sha256':'wrong','resource':1,'generation':2};self.assertFalse(valid_scope(b,p,p,10))
    def test_stop_requires_all_owned_state_cleared(self):
        w={'armed':72,'restored':72};s={'armed':6,'restored':6,'active_trap':False};self.assertTrue(valid_stop(w,s,0));self.assertFalse(valid_stop(w,s,0x80000004));s['active_trap']=True;self.assertFalse(valid_stop(w,s,0))
if __name__=='__main__':unittest.main()
