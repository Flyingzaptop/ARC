"""Name-blind GPU lowering for a proved homogeneous one-word scatter class.

The plan is static until bounded snapshot inputs, final-state recipes, and
capacity/group guards are bound. No captured process address enters HLSL.
"""
import argparse
import hashlib
import json
import shutil
import struct
from pathlib import Path

import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_REG

from cpu_composite_gpu import WIDTH, LEAVES, Unsupported, _nodes, _node_expr, _byte_expr
from cpu_composite_ir import analyze_paths
from cpu_evidence_paths import artifact_path, capture_path


def _dependencies(nodes, roots):
    found = set()
    todo = list(roots)
    while todo:
        index = todo.pop()
        if index in found:
            continue
        if not isinstance(index, int) or not 0 <= index < len(nodes):
            raise Unsupported('word DAG reference outside typed graph')
        found.add(index)
        todo.extend(ref['id'] if isinstance(ref, dict) else ref for ref in nodes[index].get('inputs', []))
    return sorted(found)


def _float(nodes, ref):
    return f'asfloat(v{ref})' if nodes[ref]['type'] in ('u8','u16','u32') else f'v{ref}'


def _float4(nodes, ref):
    return f'asfloat(v{ref})' if nodes[ref]['type'] == 'u32x4' else f'v{ref}'


def _word_expr(node, nodes, slot):
    op, typ, refs = node['op'], node['type'], node.get('inputs', [])
    if op == 'pack_bytes' and typ in ('u32', 'u64'):
        if len(refs) != WIDTH[typ]:
            raise Unsupported('scalar byte pack width')
        words = []
        for word in range(WIDTH[typ] // 4):
            parts = []
            for byte in range(4):
                ref = refs[word*4+byte]
                if not isinstance(ref, dict):
                    raise Unsupported('word pack byte source')
                parts.append(f'({_byte_expr(nodes,ref["id"],ref["byte_offset"])} << {byte*8})')
            words.append(' | '.join(parts))
        return words[0] if typ == 'u32' else f'uint2({words[0]}, {words[1]})'
    if op == 'bsf_flags' and typ == 'u8' and len(refs) == 1:
        return f'(v{refs[0]} == 0u ? 1u : 0u)'
    if op == 'bsf' and typ == 'u32' and len(refs) == 1:
        return f'firstbitlow(v{refs[0]})'
    if op == 'select' and typ == 'u32' and len(refs) == 3:
        flag, yes, no = refs
        condition = node.get('condition')
        if condition == 'nonzero':
            test = f'(v{flag} == 0u)'
        elif condition == 'above' and nodes[flag]['type'] == 'bytes3':
            test = f'((v{flag}.x | v{flag}.y) == 0u)'
        else:
            raise Unsupported('conditional select outside proved flag class')
        return f'({test} ? v{yes} : v{no})'
    if op == 'vcvtph2ps' and typ == 'f32x4' and len(refs) == 1:
        source = f'v{refs[0]}'
        if nodes[refs[0]]['type'] != 'u64':
            raise Unsupported('half source width')
        return (f'float4(f16tof32({source}.x & 65535u), f16tof32({source}.x >> 16), '
                f'f16tof32({source}.y & 65535u), f16tof32({source}.y >> 16))')
    if op in ('vmaxss','vdivss','vmulss') and typ == 'f32x4' and len(refs) == 2:
        a, b = _float4(nodes,refs[0]), _float(nodes,refs[1])
        av = f'({a}).x'
        result = (f'({av} > {b} ? {av} : {b})' if op == 'vmaxss' else
                  f'({av} / {b})' if op == 'vdivss' else f'({av} * {b})')
        return f'float4({result}, ({a}).yzw)'
    if op == 'vcvttss2si' and typ == 'u64' and len(refs) == 1:
        source = _float(nodes,refs[0])
        return f'uint2(uint(int({source})), ({source} < 0.0f) ? 0xffffffffu : 0u)'
    if op == 'btc' and typ == 'u32' and len(refs) == 2:
        return f'(v{refs[0]} ^ (1u << (v{refs[1]} & 31u)))'
    if op == 'inc' and typ == 'u32' and len(refs) == 1:
        return f'(v{refs[0]} + 1u)'
    return _node_expr(node, nodes, slot)


def _guard_expr(guard, nodes):
    predicate, source = guard['predicate'], guard.get('source')
    if not isinstance(source, int):
        raise Unsupported('unbound word guard')
    typ = nodes[source]['type']
    if predicate in ('je','jne'):
        zero = (f'((v{source}.x | v{source}.y) == 0u)' if typ == 'u64' else
                f'(v{source}.x != 0u)' if typ == 'bytes3' else
                f'(v{source} == 0u)' if typ in ('u8','u16','u32') else None)
        if zero is None:
            raise Unsupported('word guard zero-flag source')
        taken = zero if predicate == 'je' else f'(!{zero})'
    elif predicate in ('ja','jbe') and typ == 'bytes3':
        above = f'((v{source}.x | v{source}.y) == 0u)'
        taken = above if predicate == 'ja' else f'(!{above})'
    else:
        raise Unsupported('word branch predicate outside class')
    return taken if guard['taken'] else f'(!{taken})'


def _check_tail(proof, candidate):
    loop = proof['loop_bound_proof']
    if (loop.get('status') != 'verified_counted_tail' or
            loop.get('count', 0) < 1 or loop.get('step') != 16 or
            loop.get('entry_value', 0)+loop['count']*16 != loop.get('bound_value') or
            loop.get('exit_value') != loop.get('bound_value')):
        raise Unsupported('word counted-tail arithmetic')
    code = (candidate/'expected-code.bin').read_bytes()
    if hashlib.sha256(code).hexdigest() != proof.get('expected_code_sha256'):
        raise Unsupported('word expected-code identity')
    request = (candidate/'request.txt').read_text().split()
    start, size = int(request[0],16), int(request[1],16)
    if size != len(code):
        raise Unsupported('word expected-code size')
    tail = loop.get('tail') or []
    if [item.get('mnemonic') for item in tail] != ['add','cmp','jne']:
        raise Unsupported('word terminal machine path')
    meta = json.loads((candidate/'capture.json').read_text())
    decoder = capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
    decoded = []
    for item in tail:
        raw = bytes.fromhex(item['code']);offset=item['rva']-start
        if offset < 0 or code[offset:offset+len(raw)] != raw:
            raise Unsupported('word tail differs from captured code')
        ins = next(decoder.disasm(raw,meta['main_base']+item['rva'],count=1),None)
        if ins is None or ins.bytes != raw or ins.mnemonic != item['mnemonic']:
            raise Unsupported('word tail decode mismatch')
        decoded.append(ins)
    add, compare, branch = decoded
    if (len(add.operands)!=2 or add.operands[0].type!=X86_OP_REG or
            add.reg_name(add.operands[0].reg)!=loop.get('register') or
            add.operands[1].type!=X86_OP_IMM or add.operands[1].imm!=16 or
            len(compare.operands)!=2 or compare.operands[0].type!=X86_OP_REG or
            compare.reg_name(compare.operands[0].reg)!=loop.get('register') or
            compare.operands[1].type!=X86_OP_REG or
            len(branch.operands)!=1 or branch.operands[0].type!=X86_OP_IMM or
            branch.operands[0].imm!=loop.get('entry_rip') or
            branch.address+branch.size!=loop.get('exit_rip')):
        raise Unsupported('word tail register or branch target')
    return loop['count']


def generate_plan(models, group, proof, output):
    if not isinstance(models,list):models=[models]
    if len(models)<2 or group.get('status')!='observed_homogeneous_group_class' or proof.get('status')!='counted_word_scatter_snapshot_verified':
        raise Unsupported('word group or bulk proof not closed')
    candidate = capture_path(proof['candidate'])
    count = _check_tail(proof,candidate)
    typed,nodes = _nodes(models[0])
    signature=[(n['op'],n['type'],n.get('inputs',[]),n.get('control'),n.get('mask'),
                n.get('condition'),n.get('value'),n['bytes'] if n['op'] in ('constant','immediate') else None)
               for n in nodes]
    for other_model in models[1:]:
        other,other_nodes=_nodes(other_model)
        if ([(n['op'],n['type'],n.get('inputs',[]),n.get('control'),n.get('mask'),
              n.get('condition'),n.get('value'),n['bytes'] if n['op'] in ('constant','immediate') else None)
             for n in other_nodes]!=signature or
            [(o['width'],o['source']) for o in other.get('outputs',[])]!=
            [(o['width'],o['source']) for o in typed.get('outputs',[])] or
            [(g['predicate'],g['taken'],g.get('source')) for g in other.get('guards',[])[:-1]]!=
            [(g['predicate'],g['taken'],g.get('source')) for g in typed.get('guards',[])[:-1]]):
            raise Unsupported('grouped word paths differ in typed topology or guards')
    word = group['word_scatter']
    if (len(typed.get('outputs',[]))!=1 or typed['outputs'][0]['width']!=4 or
            word.get('word_stride')!=4 or word.get('first_index')!=proof['output_preflight']['entry_index'] or
            proof['output_preflight']['exit_index']-word['first_index']!=count):
        raise Unsupported('word output/index contract')
    flag_proof = group['binary_flag_select_proof']
    if (flag_proof.get('status')!='binary_select_or_structure' or
            flag_proof.get('formula_for_binary_initial_flag')!='initial_flag OR condition_a OR condition_b' or
            proof['binary_flag_preflight']['before'] not in (0,1)):
        raise Unsupported('binary flag OR preflight')
    final_state = proof.get('final_stack_metadata') or []
    if (len(final_state)!=5 or sorted(x['recipe']['kind'] for x in final_state)!=
            ['binary_initial_or_gpu_conditions','initial_plus_emitted_count']+
            ['last_item_snapshot_leaf']*3):
        raise Unsupported('final stack ranges lack five checked recipes')
    guards = typed.get('guards',[])
    if not guards or guards[-1]['predicate']!='jne' or guards[-1]['source'] is None:
        raise Unsupported('word terminal guard shape')
    if ([(g['predicate'],g['taken'],g.get('source')) for g in guards[:-1]] !=
            [(g['predicate'],g['taken'],g['source_node']) for g in group['guard_path'][:-1]]):
        raise Unsupported('group guard paths differ')
    bsf = [n for n in nodes if n['op']=='bsf']
    if len(bsf)!=1 or len(bsf[0]['inputs'])!=1:
        raise Unsupported('one bit-scan input required')
    onebit_source=bsf[0]['inputs'][0]
    roots=[typed['outputs'][0]['source'],*flag_proof['condition_source_nodes'],
           onebit_source,*[g['source'] for g in guards[:-1]]]
    reachable=_dependencies(nodes,roots)
    leaves=[i for i in reachable if nodes[i]['op'] in LEAVES]
    if any(nodes[i]['op']=='entry_register' and nodes[i].get('name') in
           (word['index_register'],group['capture_hints']['input_cursor_register'],group['capture_hints']['input_end_register'])
           for i in leaves):
        raise Unsupported('carried output index or loop induction used as per-row input')
    slots={};words=0
    for i in leaves:
        slots[i]=words;words+=(WIDTH[nodes[i]['type']]+3)//4
    if not 1<=words<=64:
        raise Unsupported('word row input budget')
    types={'u8':'uint','u16':'uint','u32':'uint','u64':'uint2','f32':'float',
           'u32x4':'uint4','f32x4':'float4','bytes3':'uint3'}
    lines=['// Typed homogeneous one-word scatter; no captured process addresses.',
           'ByteAddressBuffer Inputs : register(t0);','RWByteAddressBuffer Scratch : register(u0);',
           'cbuffer Parameters : register(b0) { uint Count; };',f'static const uint InputWords={words}u;',
           '[numthreads(64,1,1)]','void main(uint3 id : SV_DispatchThreadID) {',
           '  uint i=id.x; if(i>=Count) return;']
    for i in reachable:
        n=nodes[i];qualifier='precise ' if n['type'] in ('f32','f32x4') else ''
        lines.append(f'  {qualifier}{types[n["type"]]} v{i} = {_word_expr(n,nodes,slots.get(i))};')
    checks=[_guard_expr(g,nodes) for g in guards[:-1]]
    checks.append(f'(countbits(v{onebit_source}) == 1u)')
    # Guard the supported float->integer class before conversion can matter.
    for n in nodes:
        if n['id'] in reachable and n['op']=='vcvttss2si':
            source=n['inputs'][0]
            checks.append(f'(asfloat(v{source}) >= 0.0f && asfloat(v{source}) < 2147483648.0f)')
    lines.append('  uint valid=('+' && '.join(checks)+') ? 1u : 0u;')
    a,b=flag_proof['condition_source_nodes']
    lines.append(f'  uint eventFlag=(((v{a}.x|v{a}.y)==0u)||((v{b}.x|v{b}.y)==0u)) ? 1u : 0u;')
    lines.append('  uint at=i*5u*4u;')
    lines.append('  Scratch.Store(at,valid);')
    lines.append(f'  Scratch.Store(at+4u,valid!=0u ? v{typed["outputs"][0]["source"]} : 0u);')
    lines.append('  Scratch.Store(at+8u,valid!=0u ? 4u : 0u);')
    lines.append('  Scratch.Store(at+12u,0u);')
    lines.append('  Scratch.Store(at+16u,valid!=0u ? eventFlag : 0u);')
    lines.append('}')
    shader='\n'.join(lines)+'\n'
    output=Path(output);output.mkdir(parents=True,exist_ok=False)
    (output/'generated.hlsl').write_bytes(shader.encode())
    plan={'schema':1,'status':'static_group_shader_plan_binding_incomplete','count':count,
          'input_words_per_row':words,'scratch_words_per_row':5,'record_words_per_row':1,
          'inputs':[{'node':i,'slot':slots[i],'type':nodes[i]['type'],'origin':nodes[i]['op']} for i in leaves],
          'reachable_nodes':reachable,'word_source':typed['outputs'][0]['source'],
          'onebit_source':onebit_source,'flag_condition_nodes':flag_proof['condition_source_nodes'],
          'guard_count':len(guards)-1,'terminal_guard':'removed_by_checked_counted_tail',
          'output_index':'entry_index + GPU prefix of valid rows',
          'metadata_recipes':final_state,'initial_flag':proof['binary_flag_preflight']['before'],
          'initial_counter':proof['counter_preflight'][0]['before'],
          'initial_index':proof['output_preflight']['entry_index'],
          'shader_sha256':hashlib.sha256(shader.encode()).hexdigest(),
          'input_buffers_emitted':False,'executable_gpu_trial_allowed':False,
          'replacement_allowed':False,
          'limitations':['group membership guarded per row; invalid_count must be zero before publication',
                         'final stack metadata needs GPU reduction and packet-level last-item inputs',
                         'source ranges, input freshness, and ownership not yet bound']}
    (output/'contract.json').write_text(json.dumps(plan,indent=2)+'\n')
    return plan


def generate_compaction(proof, output):
    """Generic one-word GPU scan/scatter and explicit final-state aggregation."""
    count=proof['loop_bound_proof']['count']
    if not 1<=count<=65536 or proof['output_preflight']['exit_index']-proof['output_preflight']['entry_index']!=count:
        raise Unsupported('word compaction count/index preflight')
    states=proof.get('final_stack_metadata') or []
    last=sorted((s for s in states if s['recipe']['kind']=='last_item_snapshot_leaf'),
                key=lambda s:(s['width'],s['address']))
    if len(last)!=3 or [s['width'] for s in last]!=[1,4,8] or any(s['before_hex']!=s['after_hex'] for s in last):
        raise Unsupported('last-item stack metadata not snapshot bound')
    last1,last4,last8=last
    counter=next((s for s in states if s['recipe']['kind']=='initial_plus_emitted_count'),None)
    flag=next((s for s in states if s['recipe']['kind']=='binary_initial_or_gpu_conditions'),None)
    if (len(states)!=5 or counter is None or flag is None or
            int.from_bytes(bytes.fromhex(counter['before_hex']),'little')+count !=
            int.from_bytes(bytes.fromhex(counter['after_hex']),'little') or
            int.from_bytes(bytes.fromhex(flag['before_hex']),'little') not in (0,1)):
        raise Unsupported('counter or binary flag final-state recipe')
    output=Path(output);output.mkdir(parents=True,exist_ok=False)
    shaders={
      'prefix-local.hlsl':r'''// One-word homogeneous class: invalid is not a proven rejection.
ByteAddressBuffer Scratch:register(t0);
RWByteAddressBuffer Prefix:register(u0);
RWByteAddressBuffer GroupCounts:register(u1);
RWByteAddressBuffer GroupInvalid:register(u2);
RWByteAddressBuffer GroupFlag:register(u3);
cbuffer Parameters:register(b0){uint Count;};
groupshared uint sum[256];groupshared uint bad[256];groupshared uint flagOr[256];
[numthreads(256,1,1)]
void main(uint3 id:SV_DispatchThreadID,uint3 tid:SV_GroupThreadID,uint3 group:SV_GroupID){
 uint valid=0u,delta=0u,high=0u,flag=0u;
 if(id.x<Count){uint row=id.x*20u;valid=Scratch.Load(row);delta=Scratch.Load(row+8u);high=Scratch.Load(row+12u);flag=Scratch.Load(row+16u);}
 uint emit=(valid==1u&&delta==4u&&high==0u)?1u:0u;
 uint invalid=(id.x<Count&&emit==0u)?1u:0u;
 sum[tid.x]=emit;bad[tid.x]=invalid;flagOr[tid.x]=emit!=0u?(flag&1u):0u;
 GroupMemoryBarrierWithGroupSync();
 [unroll]for(uint step=1u;step<256u;step<<=1u){
  uint a=tid.x>=step?sum[tid.x-step]:0u;
  uint b=tid.x>=step?bad[tid.x-step]:0u;
  uint f=tid.x>=step?flagOr[tid.x-step]:0u;
  GroupMemoryBarrierWithGroupSync();
  sum[tid.x]+=a;bad[tid.x]+=b;flagOr[tid.x]|=f;
  GroupMemoryBarrierWithGroupSync();
 }
 if(id.x<Count)Prefix.Store(id.x*4u,sum[tid.x]-emit);
 if(tid.x==255u){GroupCounts.Store(group.x*4u,sum[255]);GroupInvalid.Store(group.x*4u,bad[255]);GroupFlag.Store(group.x*4u,flagOr[255]);}
}
''',
      'prefix-groups.hlsl':r'''// Single 256-lane scan covers at most 65536 input words.
ByteAddressBuffer GroupCounts:register(t0);
ByteAddressBuffer GroupInvalid:register(t1);
ByteAddressBuffer GroupFlag:register(t2);
RWByteAddressBuffer GroupOffsets:register(u0);
RWByteAddressBuffer Metadata:register(u1);
cbuffer Parameters:register(b0){uint Count;uint InitialIndex;uint InitialCounter;uint InitialFlag;
 uint LastByte;uint LastWord;uint LastQwordLo;uint LastQwordHi;};
groupshared uint sum[256];groupshared uint bad[256];groupshared uint flagOr[256];
[numthreads(256,1,1)]
void main(uint3 tid:SV_GroupThreadID){
 uint groups=(Count+255u)/256u;uint active=tid.x<groups;
 uint value=active!=0u?GroupCounts.Load(tid.x*4u):0u;
 sum[tid.x]=value;bad[tid.x]=active!=0u?GroupInvalid.Load(tid.x*4u):0u;
 flagOr[tid.x]=active!=0u?GroupFlag.Load(tid.x*4u):0u;
 GroupMemoryBarrierWithGroupSync();
 [unroll]for(uint step=1u;step<256u;step<<=1u){
  uint a=tid.x>=step?sum[tid.x-step]:0u;
  uint b=tid.x>=step?bad[tid.x-step]:0u;
  uint f=tid.x>=step?flagOr[tid.x-step]:0u;
  GroupMemoryBarrierWithGroupSync();
  sum[tid.x]+=a;bad[tid.x]+=b;flagOr[tid.x]|=f;
  GroupMemoryBarrierWithGroupSync();
 }
 if(active!=0u)GroupOffsets.Store(tid.x*4u,sum[tid.x]-value);
 if(tid.x==255u){
  uint emitted=sum[255];Metadata.Store(0u,emitted);
  Metadata.Store(4u,InitialIndex+emitted);Metadata.Store(8u,InitialCounter+emitted);
  Metadata.Store(12u,InitialFlag|flagOr[255]);Metadata.Store(16u,bad[255]);
  Metadata.Store(20u,LastWord);Metadata.Store(24u,LastQwordLo);
  Metadata.Store(28u,LastQwordHi);Metadata.Store(32u,LastByte);
 }
}
''',
      'scatter.hlsl':r'''// Output buffer starts at independently guarded entry index.
ByteAddressBuffer Scratch:register(t0);
ByteAddressBuffer Prefix:register(t1);
ByteAddressBuffer GroupOffsets:register(t2);
RWByteAddressBuffer PackedWords:register(u0);
cbuffer Parameters:register(b0){uint Count;};
[numthreads(256,1,1)]
void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Count)return;uint row=id.x*20u;
 if(Scratch.Load(row)!=1u||Scratch.Load(row+8u)!=4u||Scratch.Load(row+12u)!=0u)return;
 uint slot=GroupOffsets.Load((id.x/256u)*4u)+Prefix.Load(id.x*4u);
 PackedWords.Store(slot*4u,Scratch.Load(row+4u));
}
'''}
    for name,text in shaders.items():(output/name).write_bytes(text.encode())
    expected=struct.pack('<9I',count,proof['output_preflight']['exit_index'],
                         int.from_bytes(bytes.fromhex(counter['after_hex']),'little'),
                         int.from_bytes(bytes.fromhex(flag['after_hex']),'little'),0,
                         int.from_bytes(bytes.fromhex(last4['after_hex']),'little'),
                         int.from_bytes(bytes.fromhex(last8['after_hex'][:8]),'little'),
                         int.from_bytes(bytes.fromhex(last8['after_hex'][8:]),'little'),
                         int.from_bytes(bytes.fromhex(last1['after_hex']),'little'))
    (output/'metadata-expected.bin').write_bytes(expected)
    packet_inputs=struct.pack('<8I',count,proof['output_preflight']['entry_index'],
                              int.from_bytes(bytes.fromhex(counter['before_hex']),'little'),
                              int.from_bytes(bytes.fromhex(flag['before_hex']),'little'),
                              int.from_bytes(bytes.fromhex(last1['after_hex']),'little'),
                              int.from_bytes(bytes.fromhex(last4['after_hex']),'little'),
                              int.from_bytes(bytes.fromhex(last8['after_hex'][:8]),'little'),
                              int.from_bytes(bytes.fromhex(last8['after_hex'][8:]),'little'))
    (output/'packet-inputs.bin').write_bytes(packet_inputs)
    contract={'schema':1,'status':'isolated_word_prefix_scatter_plan','count':count,
              'scratch_words':5,'output_words_per_item':1,'metadata_words':9,
              'metadata_layout':['emitted_count','final_index','counter_after','flag_after','invalid_count',
                                 'last_word','last_qword_low','last_qword_high','last_byte'],
              'packet_inputs':{'initial_index':proof['output_preflight']['entry_index'],
                               'initial_counter':int.from_bytes(bytes.fromhex(counter['before_hex']),'little'),
                               'initial_flag':int.from_bytes(bytes.fromhex(flag['before_hex']),'little'),
                               'last_byte':expected[-4],
                               'last_word':int.from_bytes(bytes.fromhex(last4['after_hex']),'little'),
                               'last_qword':int.from_bytes(bytes.fromhex(last8['after_hex']),'little')},
              'output_slot_source':'GPU prefix of valid rows, offset from guarded entry index',
              'publication_guard':'invalid_count==0 and emitted_count==Count',
              'last_item_metadata':'three packet-level snapshot leaves, not per-row CPU indices',
              'shader_sha256':{name:hashlib.sha256((output/name).read_bytes()).hexdigest() for name in shaders},
              'packet_inputs_sha256':hashlib.sha256(packet_inputs).hexdigest(),
              'replacement_allowed':False,
              'limitations':['GPU byte validation and bounded before-view gather pending',
                             'runtime group membership, allocation ownership, and publication still unproven']}
    (output/'contract.json').write_text(json.dumps(contract,indent=2)+'\n')
    return contract


def generate_bound_fixture(models, static_plan, final_proof, compaction, output):
    """Bind exact before-view rows and independent after-view words for trial."""
    static_plan=Path(static_plan);compaction=Path(compaction)
    contract=json.loads((static_plan/'contract.json').read_text())
    if (final_proof.get('status')!='word_scatter_snapshot_gather_bound' or
            final_proof.get('source_alias_preflight',{}).get('status')!='owned_nonaliasing_before_views' or
            not final_proof.get('first_two_native_postimages_match') or
            contract.get('status')!='static_group_shader_plan_binding_incomplete'):
        raise Unsupported('bounded word snapshot gather not verified')
    binding=final_proof['artifact_binding']
    if hashlib.sha256((static_plan/'contract.json').read_bytes()).hexdigest()!=binding.get('shader_contract_sha256'):
        raise Unsupported('word static shader slot contract changed after gather')
    paths=[]
    for key,source in (('packed_input','bounded_native_before_view_gather'),
                       ('expected_words','independent_after_view_capture')):
        item=binding[key];path=artifact_path(item['path']);raw=path.read_bytes()
        if len(raw)!=item['bytes'] or hashlib.sha256(raw).hexdigest()!=item['sha256'] or item['source']!=source:
            raise Unsupported('word bulk artifact identity: '+key)
        paths.append((path,raw))
    input_bytes,expected_words=paths[0][1],paths[1][1]
    count=contract['count'];row_words=contract['input_words_per_row']
    if (len(input_bytes)!=count*row_words*4 or len(expected_words)!=count*4 or
            final_proof['source_alias_preflight']['source_range_count']!=len(final_proof['source_ranges'])):
        raise Unsupported('word full packet extent or source-range count')
    for row,model in enumerate(models[:2]):
        typed,nodes=_nodes(model)
        offset=row*row_words*4
        for item in contract['inputs']:
            raw=bytes.fromhex(nodes[item['node']]['bytes'])
            actual=input_bytes[offset+item['slot']*4:offset+item['slot']*4+len(raw)]
            if actual!=raw:
                raise Unsupported('first two word gather rows differ from typed replay')
        if expected_words[row*4:row*4+4]!=bytes.fromhex(typed['outputs'][0]['bytes']):
            raise Unsupported('first two independently captured words differ from replay')
    metadata=(compaction/'metadata-expected.bin').read_bytes()
    if len(metadata)!=36:
        raise Unsupported('five final stack recipes not represented in metadata')
    output=Path(output);output.mkdir(parents=True,exist_ok=False)
    shutil.copyfile(static_plan/'generated.hlsl',output/'generated.hlsl')
    (output/'input.bin').write_bytes(input_bytes)
    (output/'expected-words.bin').write_bytes(expected_words)
    (output/'metadata-expected.bin').write_bytes(metadata)
    result=dict(contract)
    result.update(status='isolated_word_bulk_fixture_bound',
                  input_sha256=hashlib.sha256(input_bytes).hexdigest(),
                  expected_words_sha256=hashlib.sha256(expected_words).hexdigest(),
                  metadata_sha256=hashlib.sha256(metadata).hexdigest(),
                  source_ranges=final_proof['source_ranges'],
                  artifact_binding=binding,
                  executable_gpu_trial_allowed=True,
                  live_replacement_allowed=False,
                  packet_preparation='bounded native before-view gather; cost measured separately',
                  homogeneous_guard_class_proven_for_full_count=False)
    (output/'contract.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True)
    p.add_argument('--group-contract',type=Path,required=True)
    p.add_argument('--bulk-proof',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--compaction-out',type=Path)
    a=p.parse_args();models,_=analyze_paths(a.capture)
    proof=json.loads(a.bulk_proof.read_text())
    result=generate_plan(models,json.loads(a.group_contract.read_text()),proof,a.out)
    if a.compaction_out:generate_compaction(proof,a.compaction_out)
    print(json.dumps({k:result[k] for k in ('status','count','input_words_per_row')}))


if __name__=='__main__':main()
