"""Feed measured control results; manual source bindings never choose the winner."""
import argparse,json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'scripts'))
from cpu_parallel_screen import build
def execution_conditions(gate):
    keys=set(gate['grid_gate_histogram'])
    value=False if keys=={'0'} else True if keys=={'1'} else 'mixed'
    return {'ocean_enabled':value}

def main():
    p=argparse.ArgumentParser();p.add_argument('probe',type=Path);p.add_argument('context',type=Path);p.add_argument('evidence',type=Path);a=p.parse_args()
    binding=json.loads((a.evidence/'measurement-bindings.json').read_text());conditions=dict(binding['conditions'])
    gate=json.loads((a.evidence/'gate-analysis.json').read_text())['runs']['gate']
    conditions['execution_conditions']=execution_conditions(gate)
    initial=build(a.probe,a.context,conditions=conditions)
    if initial['module_sha256']!=binding['module_sha256']:raise ValueError('Source binding generation mismatch')
    selected=initial['recommended_cheap_measurement']
    if selected!=binding['record_package']:raise ValueError('Automatic winner has no matching control measurement provider')
    if set(gate['records'])!={'65344'} or set(gate['calls_per_frame'])!={'2'}:raise ValueError('Unexpected batch class')
    measurements={selected:{'provenance':'runtime_measured','conditions':conditions,'evidence':'gate-analysis.json#/runs/gate',
        'measurement_method':'source-assisted original-loop measurement; source/binary association manually reviewed',
        'cpu_time_scope':'isolated_original_work_elapsed','cpu_useful_ms':gate['cpu_slice_median_ms'],
        'calls_per_frame':2,'batch_elements':65344,'input_bytes':65344*48,'output_bytes':65344*4,
        'replacement_lower_bound_ms':gate['prepare_median_ms'],
        'lower_bound_scope':'mandatory_input_preparation_for_declared_contract','research_attempts':1}}
    after=build(a.probe,a.context,measurements,conditions)
    following=after['recommended_cheap_measurement']
    if following==binding['grid_generation'] and conditions['execution_conditions']['ocean_enabled'] is False:measurements[following]={'provenance':'runtime_measured','conditions':conditions,
        'evidence':'gate-analysis.json#/runs/gate/grid_gate_histogram',
        'measurement_method':'source-assisted necessary-guard observation; manually reviewed binding, tested fixture only',
        'calls_per_frame':0,'batch_elements':0,'research_attempts':1}
    final=build(a.probe,a.context,measurements,conditions)
    for name,value in [('selection-before',initial),('selection-after-pack',after),('selection-after-grid',final),
                       ('parallel-measurements',{'module_sha256':initial['module_sha256'],'current_conditions':conditions,'candidates':measurements})]:
        (a.evidence/(name+'.json')).write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')
    print(selected,'->',following,'->',final['recommended_cheap_measurement'])
if __name__=='__main__':main()
