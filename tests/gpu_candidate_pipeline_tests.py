import json,math,struct,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import gpu_candidate_pipeline as p
class CandidateTests(unittest.TestCase):
    def data(self,root):
        events=[dict(event='create',event_id=1,resource=7,generation=1,bytes=65536),dict(event='selected',event_id=2,resource=7,generation=1)]
        for f in range(3):
            cpu=bytearray(131072);gpu=bytearray(65536)
            for i in range(256):
                v=struct.pack('<3f',i+1.125+f*.125,i*2+f*.25,math.sin(i+f*.2));cpu[92+i*384:104+i*384]=v;gpu[44+i*160:56+i*160]=v
            (root/f's{f}.cpu.bin').write_bytes(cpu);(root/f's{f}.gpu.bin').write_bytes(gpu)
            write=9+f*10;(root/f's{f}.windows.json').write_text(json.dumps({'frame':write,'regions':[{'address':123456,'file':f's{f}.cpu.bin'}]}))
            events.extend([dict(event='snapshot_recorded',event_id=10+f*10,snapshot=f,resource=7,generation=1,write_op=write,write_offset=0,write_bytes=65536,snapshot_bytes=65536,recording=100+f),dict(event='snapshot_submitted',event_id=11+f*10,snapshot=f,recording=100+f,queue=8,fence_value=f+1),dict(event='completed',event_id=12+f*10,snapshot=f,resource=7,generation=1,write_op=write,queue=8,fence_value=f+1,completed_value=f+1)])
        return events
    def run_case(self,mutate=lambda e:None):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);events=self.data(root);mutate(events);events.sort(key=lambda x:x['event_id']);(root/'events.jsonl').write_text('\n'.join(json.dumps(e) for e in events));return p.analyze(root)
    def test_new_write_confirms(self):self.assertEqual(self.run_case()['bindings'][0]['status'],'confirmed')
    def test_missing_completion_rejects(self):
        r=self.run_case(lambda e:e.pop());self.assertNotEqual(r['bindings'][0]['status'],'confirmed')
    def test_wrong_fence_rejects(self):
        r=self.run_case(lambda e:e[-1].update(completed_value=0));self.assertNotEqual(r['bindings'][0]['status'],'confirmed')
    def test_device_removed_fence_is_not_completion(self):
        r=self.run_case(lambda e:e[-1].update(completed_value=(1<<64)-1));self.assertNotEqual(r['bindings'][0]['status'],'confirmed')
    def test_wrong_recording_rejects(self):
        r=self.run_case(lambda e:e[-2].update(recording=999));self.assertNotEqual(r['bindings'][0]['status'],'confirmed')
    def test_alias_before_holdout_invalidates(self):
        r=self.run_case(lambda e:e.append(dict(event='invalidate',event_id=29,resource=7,generation=1,reason='aliasing')));self.assertEqual(r['bindings'][0]['status'],'invalidated_before_confirmation')
    def test_destruction_after_confirmation_clears_live_binding(self):
        r=self.run_case(lambda e:e.append(dict(event='invalidate',event_id=40,resource=7,generation=1,reason='destroyed')));self.assertEqual(r['bindings'][0]['status'],'confirmed');self.assertFalse(r['bindings'][0]['live_binding_valid'])
    def test_changed_range_is_not_merged(self):
        r=self.run_case(lambda e:e[-3].update(write_offset=4,write_bytes=65532,snapshot_bytes=65532));self.assertFalse(any(b['status']=='confirmed' for b in r['bindings']))
    def test_new_generation_is_not_merged(self):
        def change(e):e[-3]['generation']=2;e[-1]['generation']=2
        r=self.run_case(change);self.assertFalse(any(b['status']=='confirmed' for b in r['bindings']))
    def test_cpu_relocation_rejects(self):
        r={'cpu_region':'old','cpu_offset':0,'cpu_stride':16,'gpu_offset':0,'gpu_stride':16,'width':12};self.assertEqual(p.confirm(r,[{'address':'new','file':'unused'}],b'',Path('.'),b''),'cpu_placement_changed')
    def test_repeated_list_execution_rejects(self):
        r=self.run_case(lambda e:e.append(dict(event='invalid_snapshot',event_id=33,snapshot=2,reason='repeated_command_execution')));self.assertNotEqual(r['bindings'][0]['status'],'confirmed')
    def test_failed_third_data_is_not_relearned(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);events=self.data(root);(root/'s2.cpu.bin').write_bytes(bytes(131072));(root/'events.jsonl').write_text('\n'.join(json.dumps(e) for e in events));r=p.analyze(root);self.assertEqual(r['bindings'][0]['status'],'hypotheses_only');self.assertTrue(all(x['confirmation']=='holdout_mismatch' for x in r['bindings'][0]['discovery']['relations']))
    def test_unchanged_third_data_is_inconclusive(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);events=self.data(root)
            for kind in ['cpu','gpu']:(root/f's2.{kind}.bin').write_bytes((root/f's1.{kind}.bin').read_bytes())
            (root/'events.jsonl').write_text('\n'.join(json.dumps(e) for e in events));r=p.analyze(root);self.assertEqual(r['bindings'][0]['status'],'hypotheses_only');self.assertTrue(all(x['confirmation']=='inconclusive_unchanged' for x in r['bindings'][0]['discovery']['relations']))
if __name__=='__main__':unittest.main()
