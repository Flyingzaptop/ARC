import copy
import struct
import json
import os
import sys
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from cpu_composite_ir import ALIASES, Machine, Unclosed, _analyze_path, analyze
from cpu_contract_events import event_records

EVIDENCE_OVERRIDE=os.environ.get('ARC_COMPOSITE_EVIDENCE_ROOT')
DATA=Path(EVIDENCE_OVERRIDE) if EVIDENCE_OVERRIDE else Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer')


def require_capture(case,path):
    if path.exists():return
    message=f'missing {path}; set ARC_COMPOSITE_EVIDENCE_ROOT to the extracted capture root'
    if EVIDENCE_OVERRIDE:case.fail(message)
    case.skipTest(message)


class CompositeIRTests(unittest.TestCase):
    def pop_case(self,code,destination_rsp):
        initial={'mxcsr':0x1f80,'registers':[0]*16,'xmm_bytes':bytes(256),'eflags':0x202,'rip':0x2000,'code':code,'memory_known':True,'memory':[]}
        initial['registers'][4]=0x1000;raw=struct.pack('<Q',0x55667788)
        machine=Machine(initial,{'stack_low':0x1000,'stack_high':0x2000})
        tags=machine.node('constant',[],raw)
        for i,b in enumerate(raw):machine.shadow[0x1000+i]=(b,tags[i])
        nxt=copy.deepcopy(initial);nxt['registers'][4]=destination_rsp
        machine.step(initial,nxt);return machine

    def test_pop_rsp_memory_uses_postincrement_address(self):
        machine=self.pop_case('8f0424',0x1008)
        self.assertEqual(machine.writes[-1]['address'],0x1008)
        self.assertEqual(machine.writes[-1]['bytes'],'8877665500000000')

    def test_pop_rsp_register_overrides_incremented_pointer(self):
        machine=self.pop_case('5c',0x55667788)
        self.assertEqual(int.from_bytes(machine.reg['rsp'],'little'),0x55667788)

    def test_aliases(self):
        self.assertEqual(ALIASES['eax'],('rax',0,4))
        self.assertEqual(ALIASES['ah'],('rax',1,1))
        self.assertEqual(ALIASES['r15d'],('r15',0,4))

    def test_direct_callee_value_closed(self):
        path=DATA/'cpu-chain-direct-callee-20260927/candidate-00'
        require_capture(self,path)
        result=analyze(path)
        self.assertEqual(result['status'],'closed_observed_path')
        self.assertEqual(result['validated_instructions'],47)
        self.assertTrue(any(n['op']=='vcvtps2ph' for n in result['nodes']))
        self.assertTrue(result['typed_ir']['memory_accesses'])
        self.assertTrue(all(result['typed_ir']['nodes'][a['address_source']]['op']=='address'
                            for a in result['typed_ir']['memory_accesses']))
        self.assertEqual(sum(w['region']=='external' for w in result['writes']),9)
        self.assertEqual(sorted(o['width'] for o in result['typed_ir']['outputs']),[8,16])
        self.assertEqual(len(result['typed_ir']['ordered_external_writes']),9)
        self.assertFalse(result['typed_ir']['lowering_allowed'])

    def test_outer_missing_memory_fails_closed(self):
        path=DATA/'cpu-chain-iterations-20260927/candidate-01'
        require_capture(self,path)
        result=analyze(path)
        self.assertEqual(result['status'],'partial')
        self.assertEqual(result['stop']['reason'],'memory value unavailable')
        self.assertEqual(result['stop']['event'],30)

    def test_reject_training_all_paths(self):
        path=DATA/'cpu-composite-filter-train-20260927/candidate-01'
        require_capture(self,path)
        result=analyze(path)
        self.assertEqual(result['status'],'closed_observed_paths')
        self.assertEqual(result['path_count'],32)
        self.assertTrue(all(p['external_writes']==0 for p in result['paths']))
        self.assertEqual(len({tuple(map(tuple,p['branch_pattern'])) for p in result['paths']}),1)

    def test_positive_training_all_paths(self):
        path=DATA/'cpu-composite-filter-positive-20260927/candidate-01'
        require_capture(self,path)
        result=analyze(path)
        self.assertEqual(result['status'],'closed_observed_paths')
        self.assertEqual(result['path_count'],32)
        self.assertTrue(all(p['validated_instructions']==110 for p in result['paths']))
        self.assertTrue(all(p['external_writes']>0 for p in result['paths']))
        self.assertTrue(any(n['op']=='vdpps' for n in result['representative']['nodes']))
        self.assertTrue(any(n['op']=='vcvtps2ph' for n in result['representative']['nodes']))
        self.assertTrue(any(n['op']=='address' and n.get('recipe',{}).get('index')
                            for n in result['typed_ir']['nodes']))

    def test_positive_holdout_postimages(self):
        path=DATA/'cpu-composite-filter-holdout-20260927/candidate-01'
        require_capture(self,path)
        result=analyze(path)
        self.assertEqual(result['status'],'closed_observed_paths')
        self.assertTrue(all(p['post_verified_final_external_bytes']==24 for p in result['paths']))
        self.assertTrue(all(p['post_verified_writes']==16 for p in result['paths']))

    def test_corrupt_native_postimage_rejected(self):
        path=DATA/'cpu-composite-filter-holdout-20260927/candidate-01'
        require_capture(self,path)
        meta=json.loads((path/'capture.json').read_text())
        plan=json.loads((path/'memory-plan.json').read_text())
        events=list(event_records(path,meta,vectors=True))[:111]
        self.assertTrue(events[82]['previous_after'])
        target=events[82]['previous_after'][0]
        target['bytes']=('00'*target['size']) if target['bytes']!='00'*target['size'] else 'ff'*target['size']
        result=_analyze_path(events,meta,{r['rva']:r for r in plan['records']})
        self.assertEqual(result['status'],'partial')
        self.assertEqual(result['stop']['reason'],'write postimage mismatch')

    def test_grouped_mask_path(self):
        path=DATA/'cpu-chain-iterations-holdout-20260927/candidate-00'
        require_capture(self,path)
        result=analyze(path)
        self.assertEqual(result['status'],'closed_observed_path')
        self.assertEqual(result['validated_instructions'],78)
        self.assertEqual(sum(w['region']=='external' for w in result['writes']),1)
        ops={n['op'] for n in result['nodes']}
        self.assertTrue({'vcvtph2ps','vdivss','vmaxss','vcvttss2si','bsf','btc'}<=ops)
        self.assertTrue(all(n['type']=='f32x4' for n in result['typed_ir']['nodes']
                            if n['op'] in ('vcvtph2ps','vdivss','vmaxss','vmulss')))


if __name__=='__main__':unittest.main()
