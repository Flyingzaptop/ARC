"""Describe captured input gathering and loop-carried state from composite IR.

The recipe is structural evidence for an isolated snapshot replay. It contains
no executable pointer dereference and grants no live memory ownership.
"""
import argparse
import hashlib
import json
import struct
from collections import defaultdict
from pathlib import Path

import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_REG

from cpu_composite_ir import analyze_paths
from cpu_producer_plan import GPRS


class Unresolved(ValueError):
    pass


def _dependencies(nodes,root):
    seen=set();todo=[root]
    while todo:
        node_id=todo.pop()
        if node_id is None or node_id in seen:continue
        if not isinstance(node_id,int) or not 0<=node_id<len(nodes):raise Unresolved('invalid typed DAG edge')
        seen.add(node_id)
        for operand in nodes[node_id]['inputs']:
            todo.append(operand if isinstance(operand,int) else operand['id'])
    return seen


def _variation(values):
    if len(set(values))==1:return {'pattern':'same_observed_value','first':values[0]}
    if all(isinstance(v,int) for v in values):
        delta=values[1]-values[0]
        if all(b-a==delta for a,b in zip(values,values[1:])):
            return {'pattern':'affine_observed_values','first':values[0],'delta':delta}
    return {'pattern':'varied_observed_values','first':values[0]}


def _tail_steps(groups):
    decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
    steps=[]
    for events in groups:
        found=[]
        for event in events[-8:-1]:
            x=next(decoder.disasm(bytes.fromhex(event['code']),event['rip']),None)
            if x and x.mnemonic=='add' and len(x.operands)==2 and x.operands[0].type==X86_OP_REG and x.operands[1].type==X86_OP_IMM:
                found.append((x.reg_name(x.operands[0].reg),x.operands[1].imm))
        steps.append(found)
    if not steps or any(step!=steps[0] for step in steps):return []
    return [{'register':name,'step':step,'observed_in_tail':True} for name,step in steps[0]]


def _entry_progressions(groups):
    starts=[events[0]['registers'] for events in groups]
    ends=[events[-1]['registers'] for events in groups]
    result=[]
    for index,name in enumerate(GPRS):
        values=[state[index] for state in starts]
        if len(values)<2:continue
        delta=(values[1]-values[0])&((1<<64)-1)
        if delta and all((b-a)&((1<<64)-1)==delta for a,b in zip(values,values[1:])):
            carried=all(ends[j][index]==starts[j+1][index] for j in range(len(groups)-1))
            result.append({'register':name,'start':values[0],'step_unsigned':delta,
                           'matches_previous_path_exit':carried})
    return result


def derive(candidate):
    models,groups=analyze_paths(candidate)
    if len(models)<2 or any(model['status']!='closed_observed_path' for model in models):
        raise Unresolved('multiple fully value-closed paths required')
    typed=[model['typed_ir'] for model in models]
    reference=typed[0];nodes=reference['nodes']
    topology=[(node['op'],node['type'],node['inputs']) for node in nodes]
    access_shape=[(a['kind'],a['size'],a['address_source'],a['value_source']) for a in reference['memory_accesses']]
    for path in typed[1:]:
        if [(n['op'],n['type'],n['inputs']) for n in path['nodes']]!=topology:
            raise Unresolved('path typed topology diverges')
        if [(a['kind'],a['size'],a['address_source'],a['value_source']) for a in path['memory_accesses']]!=access_shape:
            raise Unresolved('path access topology diverges')
    # Output values, branch guards, and all external write addresses are needed.
    roots=[atom['source'] for atom in reference['output_atoms']]
    roots.extend(guard['source'] for guard in reference['guards'] if guard['source'] is not None)
    roots.extend(a['address_source'] for a in reference['memory_accesses']
                 if a['kind']=='write' and not candidate_is_stack(a['address'],models[0],candidate))
    required=set().union(*(_dependencies(nodes,root) for root in roots))
    read_accesses=reference['memory_accesses']
    read_origin={}
    for index,access in enumerate(read_accesses):
        if access['kind']!='read':continue
        for leaf in _dependencies(nodes,access['value_source']):
            if nodes[leaf]['op']=='memory_input':read_origin.setdefault(leaf,index)
    while True:
        expanded=set(required)
        for leaf in required:
            if nodes[leaf]['op']=='memory_input' and leaf in read_origin:
                expanded.update(_dependencies(nodes,read_accesses[read_origin[leaf]]['address_source']))
        if expanded==required:break
        required=expanded
    memory_slots=[]
    for node in nodes:
        if node['op']!='memory_input':continue
        node_id=node['id']
        access_index=read_origin.get(node_id)
        if access_index is None:raise Unresolved('captured memory leaf lacks access')
        accesses=[path['memory_accesses'][access_index] for path in typed]
        addresses=[a['address'] for a in accesses]
        address_root=accesses[0]['address_source']
        address_deps=_dependencies(nodes,address_root)
        pointer_leaves=sorted(i for i in address_deps if nodes[i]['op']=='memory_input')
        entry_roots=sorted(i for i in address_deps if nodes[i]['op'] in ('entry_register','entry_vector','captured_teb','code_address'))
        memory_slots.append({'slot':node_id,'access_index':access_index,
                             'value_source':accesses[0]['value_source'],
                             'width':len(bytes.fromhex(node['bytes'])),
                             'needed_for_output_or_guard':node_id in required,
                             'address_source':address_root,'address_recipe':node.get('address_recipe'),
                             'observed_address':_variation(addresses),
                             'observed_bytes_equal':len({path['nodes'][node_id]['bytes'] for path in typed})==1,
                             'pointer_dependencies':pointer_leaves,
                             'root_live_ins':entry_roots,
                             'availability':'captured_stack_live_in' if candidate_is_stack(addresses[0],models[0],candidate)
                                             else 'captured_external_input'})
    # A read of a prior path's external write is loop-carried state, not a
    # fresh independent input for every element.
    carried=[]
    for current in range(1,len(models)):
        previous=model_external_final(models[current-1])
        for slot in memory_slots:
            address=typed[current]['memory_accesses'][read_origin[slot['slot']]]['address']
            width=slot['width']
            if all(address+j in previous for j in range(width)):
                expected=bytes(previous[address+j] for j in range(width)).hex()
                actual=typed[current]['nodes'][slot['slot']]['bytes']
                if expected==actual:
                    carried.append({'path':current,'slot':slot['slot'],'address':address,'width':width})
    for slot in memory_slots:
        hits=[edge for edge in carried if edge['slot']==slot['slot']]
        if len(hits)==len(models)-1:
            slot['availability']='loop_carried_after_first_path'
            slot['initial_seed_required']=True
        elif hits:slot['availability']='mixed_external_and_observed_loop_carried'
    entry_inputs=[]
    for node in nodes:
        if node['op'] not in ('entry_register','entry_vector','captured_teb','code_address'):continue
        entry_inputs.append({'node':node['id'],'kind':node['op'],'name':node.get('name'),
                             'needed_for_output_or_guard':node['id'] in required,
                             'observed_bytes_equal':len({path['nodes'][node['id']]['bytes'] for path in typed})==1,
                             'binding':'explicit_captured_live_in_only'})
    writes=[]
    carried_slots={s['slot'] for s in memory_slots if s['availability']=='loop_carried_after_first_path'}
    for i,access in enumerate(reference['memory_accesses']):
        if access['kind']!='write' or candidate_is_stack(access['address'],models[0],candidate):continue
        address_deps=_dependencies(nodes,access['address_source'])
        value_deps=_dependencies(nodes,access['value_source'])
        pointer_leaves=sorted(j for j in address_deps if nodes[j]['op']=='memory_input')
        writes.append({'access_index':i,'width':access['size'],'address_source':access['address_source'],
                       'value_source':access['value_source'],
                       'pointer_dependencies':pointer_leaves,
                       'address_depends_on_loop_carried_cursor':bool(address_deps&carried_slots),
                       'value_depends_on_loop_carried_cursor':bool(value_deps&carried_slots),
                       'observed_address':_variation([path['memory_accesses'][i]['address'] for path in typed])})
    address_ids=set().union(*(_dependencies(nodes,a['address_source']) for a in reference['memory_accesses']))
    return {'schema':1,'status':'captured_dependency_recipe','path_count':len(models),
            'typed_topology_identical':True,'iteration_tail_steps':_tail_steps(groups),
            'entry_progressions':_entry_progressions(groups),
            'memory_inputs':memory_slots,'entry_live_ins':entry_inputs,
            'ordered_external_writes':writes,'observed_cross_path_edges':carried,
            'independent_output_scatter_supported':not any(w['address_depends_on_loop_carried_cursor'] or
                                                         w['value_depends_on_loop_carried_cursor'] for w in writes),
            'parallel_output_obligation':'derive selected-element prefix positions and publish cursor once in order' if carried else None,
            'address_dag':[node for node in nodes if node['id'] in address_ids],
            'live_gather_allowed':False,'executable_gather_emitted':False,
            'unresolved':['bounds and allocation generations for pointer-chasing roots',
                          'live freshness and concurrent access','GPU producer or upload route for external leaves',
                          'prefix/compaction semantics for loop-carried append cursor']}


def candidate_is_stack(address,model,candidate):
    meta=json.loads((candidate/'capture.json').read_text())
    return meta.get('stack_low',0)<=address<meta.get('stack_high',0)


def model_external_final(model):
    values={}
    for output in model['typed_ir']['outputs']:
        raw=bytes.fromhex(output['bytes'])
        for j,b in enumerate(raw):values[output['address']+j]=b
    return values


def derive_stateful_packet(candidate):
    """Find observed stack/group state edges without assuming independent items."""
    candidate=Path(candidate);models,groups=analyze_paths(candidate)
    meta=json.loads((candidate/'capture.json').read_text())
    low,high=meta.get('stack_low',0),meta.get('stack_high',0)
    if low>=high:raise Unresolved('stack bounds unavailable')
    summaries=[];states=[]
    for model in models:
        if model['status']!='closed_observed_path':raise Unresolved('state path not value closed')
        typed=model['typed_ir'];written=set();last={};preexisting=[];writes=[]
        for index,access in enumerate(typed['memory_accesses']):
            address,size=access['address'],access['size']
            if not low<=address or address+size>high:continue
            raw=bytes.fromhex(typed['nodes'][access['value_source']]['bytes'])
            if len(raw)!=size:raise Unresolved('stack access value width')
            if access['kind']=='read' and any(address+j not in written for j in range(size)):
                preexisting.append({'access_index':index,'address':address,'width':size,'bytes':raw.hex(),
                                    'address_source':access['address_source'],'value_source':access['value_source']})
            elif access['kind']=='write':
                writes.append({'access_index':index,'address':address,'width':size,'bytes':raw.hex(),
                               'address_source':access['address_source'],'value_source':access['value_source']})
                for j,b in enumerate(raw):written.add(address+j);last[address+j]=b
        external=[w for w in model['writes'] if w['region']=='external']
        summaries.append({'first_event':groups[len(summaries)][0]['index'],
                          'validated_instructions':model['validated_instructions'],
                          'branch_pattern':[(g['predicate'],g['taken']) for g in model['guards']],
                          'external_write_count':len(external),
                          'external_write_addresses':[w['address'] for w in external],
                          'preexisting_stack_reads':preexisting,'stack_writes':writes,
                          'post_verified_final_external_bytes':model['post_verified_final_external_bytes']})
        states.append(last)
    edges=[]
    for path in range(1,len(summaries)):
        previous=states[path-1]
        for read in summaries[path]['preexisting_stack_reads']:
            address,size=read['address'],read['width']
            if all(address+j in previous for j in range(size)):
                same=bytes(previous[address+j] for j in range(size)).hex()==read['bytes']
                edges.append({'from_path':path-1,'to_path':path,'address':address,
                              'width':size,'native_values_equal':same})
    return {'schema':1,'status':'observed_stateful_paths','path_count':len(models),
            'paths':summaries,'observed_cross_path_stack_edges':edges,
            'external_writes_total':sum(p['external_write_count'] for p in summaries),
            'independent_items_proven':False,
            'group_boundaries_proven':False,
            'unresolved':['which stack live-ins initialize each group','all branch classes and mask cardinalities',
                          'global append capacity, publication, ownership, and consumer deadline']}


def derive_homogeneous_group_contract(candidate):
    """Narrow observed same-guard class with carried counter and word scatter."""
    candidate=Path(candidate);models,groups=analyze_paths(candidate)
    capture_meta=json.loads((candidate/'capture.json').read_text())
    if len(models)<2 or any(m['status']!='closed_observed_path' for m in models):
        raise Unresolved('multiple closed group paths required')
    state=derive_stateful_packet(candidate);patterns=[p['branch_pattern'] for p in state['paths']]
    if any(p!=patterns[0] for p in patterns[1:]):raise Unresolved('heterogeneous branch class')
    if any(p['external_write_count']!=1 or p['post_verified_final_external_bytes']!=4 for p in state['paths']):
        raise Unresolved('word write or native postimage unavailable')
    addresses=[p['external_write_addresses'][0] for p in state['paths']]
    if any(b-a!=4 for a,b in zip(addresses,addresses[1:])):
        raise Unresolved('output addresses not contiguous words')
    first=models[0]['typed_ir'];output=first['outputs'][0]
    access=next(a for a in first['memory_accesses'] if a['kind']=='write' and a['address']==output['address'] and a['size']==4)
    address_node=first['nodes'][access['address_source']]
    if address_node['op']!='address' or address_node['recipe']['scale']!=4 or len(address_node['inputs'])!=2:
        raise Unresolved('word scatter address recipe')
    index_deps=_dependencies(first['nodes'],address_node['inputs'][1])
    entry_indices=[i for i in index_deps if first['nodes'][i]['op']=='entry_register']
    if len(entry_indices)!=1:raise Unresolved('word scatter index root')
    register=first['nodes'][entry_indices[0]]['name'];slot=GPRS.index(register)
    starts=[events[0]['registers'][slot] for events in groups]
    ends=[events[-1]['registers'][slot] for events in groups]
    if any(b-a!=1 for a,b in zip(starts,starts[1:])) or any(ends[i]!=starts[i+1] for i in range(len(starts)-1)):
        raise Unresolved('scatter index progression')
    base=addresses[0]-starts[0]*4
    if any(address!=base+index*4 for address,index in zip(addresses,starts)):
        raise Unresolved('scatter base not stable')
    store_events=[]
    for events,expected_address in zip(groups,addresses):
        match=[]
        for event in events[:-1]:
            x=next(capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64).disasm(bytes.fromhex(event['code']),event['rip']),None)
            if not x or x.mnemonic!='mov' or not x.op_str.startswith('dword ptr ['):continue
            if any(item['address']==expected_address and item['size']==4 and item['ok'] for item in event.get('memory',[])):
                match.append((event['rip']-capture_meta['main_base'],event['code'][:x.size*2],x.op_str))
        if len(match)!=1:raise Unresolved('external word store instruction ambiguous')
        store_events.append(match[0])
    if any(item!=store_events[0] for item in store_events[1:]):
        raise Unresolved('word store instruction changed')
    base_deps=_dependencies(first['nodes'],address_node['inputs'][0])
    pointer_leaves=[i for i in base_deps if first['nodes'][i]['op']=='memory_input' and
                    len(bytes.fromhex(first['nodes'][i]['bytes']))==8]
    if len(pointer_leaves)!=1:raise Unresolved('output base pointer leaf ambiguous')
    pointer_node=first['nodes'][pointer_leaves[0]]
    decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
    tail=[next(decoder.disasm(bytes.fromhex(event['code']),event['rip']),None) for event in groups[0][-4:-1]]
    if any(x is None for x in tail) or [x.mnemonic for x in tail]!=['add','cmp','jne']:
        raise Unresolved('input tail not counted')
    add,compare,branch=tail
    if (add.operands[0].type!=X86_OP_REG or add.operands[1].type!=X86_OP_IMM or
        compare.operands[0].type!=X86_OP_REG or compare.operands[1].type!=X86_OP_REG):
        raise Unresolved('input tail operands')
    cursor_reg=add.reg_name(add.operands[0].reg);end_reg=compare.reg_name(compare.operands[1].reg)
    step=add.operands[1].imm
    if compare.reg_name(compare.operands[0].reg)!=cursor_reg or step<=0:
        raise Unresolved('input cursor mismatch')
    entry=groups[0][0]['registers'];cursor=entry[GPRS.index(cursor_reg)];end=entry[GPRS.index(end_reg)]
    if end<cursor or (end-cursor)%step or any(events[0]['registers'][GPRS.index(cursor_reg)]!=cursor+i*step for i,events in enumerate(groups)):
        raise Unresolved('input cursor progression')
    stack_edges=state['observed_cross_path_stack_edges']
    if not stack_edges or any(not edge['native_values_equal'] for edge in stack_edges):
        raise Unresolved('carried stack value mismatch')
    counter_candidates=[]
    for write in state['paths'][0]['stack_writes']:
        if write['width']!=4:continue
        address=write['address']
        values=[]
        for path in state['paths']:
            match=next((w for w in path['stack_writes'] if w['address']==address and w['width']==4),None)
            if match is None:break
            values.append(int.from_bytes(bytes.fromhex(match['bytes']),'little'))
        if len(values)==len(models) and all(b-a==1 for a,b in zip(values,values[1:])):
            counter_candidates.append({'address':address,'first_after':values[0],
                                       'last_after':values[-1],'observed_step':1})
    guards=[]
    for guard in first['guards']:
        source=guard.get('source')
        leaves=[] if source is None else sorted(i for i in _dependencies(first['nodes'],source)
                                              if first['nodes'][i]['op'] in ('memory_input','entry_register','entry_vector'))
        guards.append({'predicate':guard['predicate'],'taken':guard['taken'],
                       'source_node':source,'input_leaves':leaves,
                       'leaf_bindings':[{'node':i,'op':first['nodes'][i]['op'],
                                         'width':len(bytes.fromhex(first['nodes'][i]['bytes'])),
                                         'address_recipe':first['nodes'][i].get('address_recipe'),
                                         'entry_name':first['nodes'][i].get('name')}
                                        for i in leaves]})
    conditionals=first.get('conditionals',[])
    select_proof=None
    def low_origin(node_id):
        seen=set();offset=0
        while True:
            if (node_id,offset) in seen:raise Unresolved('byte provenance cycle')
            seen.add((node_id,offset));node=first['nodes'][node_id]
            if node['op']=='pack_bytes':
                ref=node['inputs'][offset];node_id,offset=ref['id'],ref['byte_offset']
            elif node['op']=='extract_bytes':
                offset+=node['byte_offset'];node_id=node['inputs'][0]
            else:return node_id,offset
    cmova=[c for c in conditionals if c['predicate']=='cmova']
    if len(cmova)==2:
        a,b=[first['nodes'][c['source']] for c in cmova]
        if (a['op']=='select' and b['op']=='select' and a['condition']==b['condition']=='above' and
            a['inputs'][1]==b['inputs'][1] and first['nodes'][a['inputs'][1]]['op']=='immediate' and
            int.from_bytes(bytes.fromhex(first['nodes'][a['inputs'][1]]['bytes']),'little')==1 and
            low_origin(b['inputs'][2])==(a['id'],0)):
            old_id,old_offset=low_origin(a['inputs'][2]);old=first['nodes'][old_id]
            if old_offset==0 and old['op']=='memory_input' and len(bytes.fromhex(old['bytes']))==1:
                stack_address=old['address']
                last_write=next((access for access in reversed(first['memory_accesses'])
                                 if access['kind']=='write' and access['address']==stack_address and access['size']==1),None)
                if last_write and low_origin(last_write['value_source'])==(b['id'],0):
                    observed={model['typed_ir']['nodes'][old_id]['bytes'] for model in models}
                    if all(int(value,16) in (0,1) for value in observed):
                        select_proof={'status':'binary_select_or_structure',
                                      'first_select_node':a['id'],'second_select_node':b['id'],
                                      'condition_source_nodes':[a['inputs'][0],b['inputs'][0]],
                                      'selected_one_node':a['inputs'][1],
                                      'initial_flag_leaf':old_id,'final_flag_address':stack_address,
                                      'observed_initial_flag_values':sorted(int(v,16) for v in observed),
                                      'formula_for_binary_initial_flag':'initial_flag OR condition_a OR condition_b',
                                      'runtime_preflight_required':'initial_flag in {0,1}',
                                      'runtime_membership_guard_enforced':False}
    return {'schema':1,'status':'observed_homogeneous_group_class','path_count':len(models),
            'guard_path':guards,'conditional_selects':conditionals,
            'binary_flag_select_proof':select_proof,
            'group_membership_enforced':False,
            'word_scatter':{'base_observed':base,'index_register':register,'first_index':starts[0],
                            'last_index':starts[-1],'word_stride':4,'observed_addresses':addresses,
                            'store_rva':store_events[0][0],'store_code':store_events[0][1],
                            'store_operands':store_events[0][2],
                            'address_recipe':address_node['recipe'],
                            'address_source_node':access['address_source'],
                            'native_postimage_bytes_verified':4*len(models)},
            'counter_candidates':counter_candidates,
            'carried_stack_edges':stack_edges,
            'capture_hints':{'output_base_pointer_leaf':pointer_leaves[0],
                             'output_base_pointer_read_address':pointer_node['address'],
                             'output_base_pointer_address_recipe':pointer_node.get('address_recipe'),
                             'output_base_observed':base,
                             'input_cursor_register':cursor_reg,'input_end_register':end_reg,
                             'input_cursor_observed':cursor,'input_end_observed':end,
                             'input_stride':step,'remaining_elements_observed':(end-cursor)//step,
                             'tail':[{'rva':x.address-capture_meta['main_base'],'code':bytes(x.bytes).hex(),
                                      'mnemonic':x.mnemonic,'operands':x.op_str} for x in tail],
                             'scope':'pointer and span hints for bounded capture only'},
            'stack_write_recipes':state['paths'][0]['stack_writes'],
            'no_growth_state_scope':'output pointer and capacity bounds not yet established for this group',
            'independent_items_proven':False,'replacement_allowed':False,
            'required_runtime_guards':['same branch class and group membership for every item',
                                       'bounded bit-mask cardinality and ordered word scatter',
                                       'counter/flag initial state and exclusive ownership',
                                       'output capacity, publication deadline, and consumer mapping']}


def verify_group_bulk_views(candidate):
    """Capture expected words and carried metadata from owned before/after views."""
    candidate=Path(candidate);contract=derive_homogeneous_group_contract(candidate)
    views=json.loads((candidate/'views.json').read_text())
    if not views['completed'] or views['missed_regions'] or not all(r['before_ok'] and r['after_ok'] for r in views['regions']):
        raise Unresolved('incomplete group snapshot views')
    meta=json.loads((candidate/'capture.json').read_text())
    entry=(candidate/views['entry_context_file']).read_bytes();exit_=(candidate/views['exit_context_file']).read_bytes()
    if len(entry)!=meta['context_bytes'] or len(exit_)!=meta['context_bytes']:
        raise Unresolved('group context size')
    def reg(data,name):return struct.unpack_from('<Q',data,meta['rax_offset']+GPRS.index(name)*8)[0]
    hints=contract['capture_hints'];start=reg(entry,hints['input_cursor_register'])
    end=reg(entry,hints['input_end_register']);final=reg(exit_,hints['input_cursor_register'])
    first=reg(entry,contract['word_scatter']['index_register'])
    last=reg(exit_,contract['word_scatter']['index_register'])
    count=(end-start)//hints['input_stride']
    if (start!=hints['input_cursor_observed'] or end!=hints['input_end_observed'] or final!=end or
        (end-start)%hints['input_stride'] or last-first!=count):
        raise Unresolved('group bulk cursor/index count mismatch')
    def read(address,size,phase):
        matching=[r for r in views['regions'] if r['base']<=address and address+size<=r['base']+r['size']]
        if len(matching)!=1:raise Unresolved('group view range unavailable')
        r=matching[0];path=candidate/r[phase+'_file']
        if path.stat().st_size!=r['size']:raise Unresolved('group view byte count')
        with path.open('rb') as stream:
            stream.seek(address-r['base']);data=stream.read(size)
        if len(data)!=size:raise Unresolved('group view short read')
        return data
    base=hints['output_base_observed'];output_start=base+first*4;output_bytes=count*4
    before=read(output_start,output_bytes,'before');after=read(output_start,output_bytes,'after')
    models,_=analyze_paths(candidate)
    for i,model in enumerate(models[:2]):
        if after[i*4:i*4+4]!=bytes.fromhex(model['typed_ir']['outputs'][0]['bytes']):
            raise Unresolved('first two group words disagree with after view')
    counters=[]
    for item in contract['counter_candidates']:
        a=item['address'];before_value=int.from_bytes(read(a,4,'before'),'little')
        after_value=int.from_bytes(read(a,4,'after'),'little')
        if after_value-before_value!=count:raise Unresolved('group counter delta mismatch')
        counters.append({'address':a,'before':before_value,'after':after_value,'delta':count})
    flag_address=contract['binary_flag_select_proof']['final_flag_address']
    flag_before=read(flag_address,1,'before')[0];flag_after=read(flag_address,1,'after')[0]
    if flag_before not in (0,1) or flag_after not in (0,1):raise Unresolved('group flag outside binary class')
    return {'schema':1,'status':'bulk_group_snapshot_consistent','count':count,
            'entry_index':first,'exit_index':last,'input_stride':hints['input_stride'],
            'output_word_base':base,'output_start':output_start,'output_bytes':output_bytes,
            'output_after_sha256':hashlib.sha256(after).hexdigest(),
            'changed_words':sum(before[j:j+4]!=after[j:j+4] for j in range(0,output_bytes,4)),
            'first_two_native_postimages_match':True,
            'counters':counters,'binary_flag':{'address':flag_address,'before':flag_before,'after':flag_after},
            'homogeneous_guard_class_proven_for_full_count':False,
            'snapshot_coherent_proven':False,'live_replacement_allowed':False}


def main():
    p=argparse.ArgumentParser();p.add_argument('candidate',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    result=derive(a.candidate);a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'status':result['status'],'paths':result['path_count'],'memory_inputs':len(result['memory_inputs']),
                      'loop_carried_edges':len(result['observed_cross_path_edges'])}))


if __name__=='__main__':main()
