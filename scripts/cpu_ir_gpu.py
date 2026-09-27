"""Lower a captured scalar-f32 provenance graph to an isolated HLSL experiment.

This is code generation from replay evidence, not a live replacement contract.
Addresses identify captured leaves within one call; the generated interface uses
ordinal slots and has no game addresses, symbols, or field names.
"""
import argparse
import hashlib
import json
import math
import struct
from pathlib import Path


class Unsupported(ValueError):
    pass


def _f32(raw):
    value = struct.unpack('<f', raw)[0]
    if not math.isfinite(value) or (value != 0 and abs(value) < 2**-126):
        raise Unsupported('non-normal or non-finite float32 input')
    return value


def _word(raw):
    return struct.unpack('<I', raw)[0]


def _compile_call(call):
    if call.get('status') != 'exact_output_replay':
        raise Unsupported('call lacks exact replay')
    captured = {}
    for item in call['input_memory']:
        raw = bytes.fromhex(item['bytes'])
        if len(raw) != 4:
            raise Unsupported('only four-byte captured inputs are supported')
        address = item['address']
        if address in captured and captured[address] != raw:
            raise Unsupported('one address has multiple captured values')
        captured[address] = raw
    slots = {}
    values = []
    operations = []
    memo = {}

    def scalar(tag):
        if not isinstance(tag, dict):
            raise Unsupported('malformed expression node')
        if tag.get('kind') == 'memory_input':
            if tag.get('bytes') != 4 or set(tag) != {'kind', 'address', 'bytes'}:
                raise Unsupported('unsupported memory leaf')
            address = tag['address']
            if address not in captured:
                raise Unsupported('leaf lacks captured input')
            if address not in slots:
                slots[address] = len(values)
                values.append(captured[address])
            return ('input', slots[address])
        if tag.get('kind') is not None:
            raise Unsupported('unsupported leaf kind ' + str(tag['kind']))
        if tag.get('op') not in ('vaddss', 'vmulss') or tag.get('precision') != 'float32':
            raise Unsupported('unsupported operation or precision')
        for side in ('left', 'right'):
            lanes = tag.get(side)
            if not isinstance(lanes, list) or len(lanes) != 4 or any(lane != lanes[0] for lane in lanes):
                raise Unsupported('scalar operand has mixed byte provenance')
        left = scalar(tag['left'][0]); right = scalar(tag['right'][0])
        key = (tag['op'], left, right)
        if key not in memo:
            memo[key] = len(operations)
            operations.append(key)
        return ('temp', memo[key])

    output = bytes.fromhex(call['output'])
    if len(output) == 0 or len(output) % 4:
        raise Unsupported('output is not whole float32 words')
    words = [None] * (len(output)//4)
    for part in call['output_provenance']:
        offset = part['offset']
        if offset % 4 or part['bytes'] != 4 or offset < 0 or offset + 4 > len(output):
            raise Unsupported('output map is not scalar float32')
        index = offset//4
        if words[index] is not None:
            raise Unsupported('overlapping output map')
        words[index] = scalar(part['expression'])
    if any(word is None for word in words):
        raise Unsupported('incomplete output map')
    if set(captured) != set(slots):
        raise Unsupported('unused captured memory input')
    shape = {'inputs':len(values), 'operations':operations, 'outputs':words}
    return shape, values, output, list(slots)


def _evaluate(shape, values):
    floats = [_f32(raw) for raw in values]
    temporaries = []
    def get(ref):
        return floats[ref[1]] if ref[0] == 'input' else temporaries[ref[1]]
    for op,left,right in shape['operations']:
        a,b = get(left),get(right)
        result = a+b if op == 'vaddss' else a*b
        try:
            raw = struct.pack('<f',result)
        except OverflowError as exc:
            raise Unsupported('float32 overflow') from exc
        value = struct.unpack('<f',raw)[0]
        if not math.isfinite(value) or (value != 0 and abs(value) < 2**-126):
            raise Unsupported('non-normal arithmetic output')
        temporaries.append(value)
    return b''.join(struct.pack('<f',get(ref)) for ref in shape['outputs'])


def _shader(shape):
    count = shape['inputs']; outputs = len(shape['outputs'])
    def ref(item):
        return ('v' if item[0]=='input' else 't') + str(item[1])
    lines = [
        '// Generated from captured provenance; scalar IEEE float32 class only.',
        'ByteAddressBuffer Inputs : register(t0);',
        'RWByteAddressBuffer Outputs : register(u0);',
        'cbuffer Parameters : register(b0) { uint Count; };',
        '[numthreads(64,1,1)]',
        'void main(uint3 id : SV_DispatchThreadID) {',
        '  uint i = id.x; if (i >= Count) return;',
    ]
    for n in range(count):
        lines.append(f'  precise float v{n} = asfloat(Inputs.Load((i * {count} + {n}) * 4));')
    for n,(op,left,right) in enumerate(shape['operations']):
        symbol = '+' if op == 'vaddss' else '*'
        lines.append(f'  precise float t{n} = {ref(left)} {symbol} {ref(right)};')
    for n,item in enumerate(shape['outputs']):
        lines.append(f'  Outputs.Store((i * {outputs} + {n}) * 4, asuint({ref(item)}));')
    lines.append('}')
    return '\n'.join(lines)+'\n'


def _packet_calls(root, group_filter=None):
    # Reuse the same replay and affine relocation evidence as analyze_cpu_batch.
    from cpu_producer_replay import run as replay_run
    bp=json.loads((root/'batch/plan.json').read_text())
    groups=json.loads((root/'batch/groups.json').read_text())
    if not groups or not all(g['rows'] for g in groups):
        raise Unsupported('empty packet capture')
    observed=replay_run(root/'slice')
    if not observed['exact'] or not observed['holdout_passed']:
        raise Unsupported('packet slice replay failed')
    call=observed['calls'][0]
    shape,_,_,addresses=_compile_call(call)
    trace=[json.loads(x) for x in (root/'slice/trace.jsonl').read_text().splitlines()]
    entry=next(x for x in trace if x['call']==call['call'] and x['kind']==1)
    source=entry['registers'][bp['producer_input_reg']]
    constants={item['address']:bytes.fromhex(item['bytes']) for item in call['input_memory'] if not source<=item['address']<source+bp['input_span']}
    packets=[]
    descriptors=[];shared=[]
    for address in addresses:
        if source<=address<=source+bp['input_span']-4:
            descriptors.append(address-source)
        elif address in constants:
            descriptors.append(-1-len(shared));shared.append(constants[address])
        else:
            raise Unsupported('unresolved packet input descriptor')
    stride=bp['input_stride']
    if stride<bp['input_span'] or stride>4096:
        raise Unsupported('unsupported packet stride')
    source_rows=[]
    for group_index,group in enumerate(groups):
        if group_filter is not None and group_index != group_filter:
            continue
        if [r['index'] for r in group['rows']]!=list(range(group['start'],group['end'])):
            raise Unsupported('packet indices not contiguous')
        for row in group['rows']:
            i=row['index']
            if i!=row['argument_index'] or row['input_address']!=bp['expected_input_base']+i*bp['input_stride'] or row['output_address']!=bp['expected_output_base']+i*bp['output_stride']:
                raise Unsupported('packet affine address mismatch')
            if not row['callback_begin_qpc']<=row['producer_qpc']<=row['callback_end_qpc']<=group['end_qpc']:
                raise Unsupported('packet timing order mismatch')
            data=bytes.fromhex(row['input_ready'])
            if len(data)!=bp['input_span']:
                raise Unsupported('packet source span mismatch')
            source_rows.append(data.ljust(stride,b'\0'))
            values=[]
            for address in addresses:
                if source<=address<=source+len(data)-4:
                    values.append(data[address-source:address-source+4])
                elif address in constants:
                    values.append(constants[address])
                else:
                    raise Unsupported('unresolved relocated packet input')
            expected=bytes.fromhex(row['output_after'])
            if _evaluate(shape,values)!=expected:
                raise Unsupported('packet CPU reference differs at element '+str(i))
            packets.append((shape,values,expected,{'call':i,'role':'holdout' if group_index==len(groups)-1 else 'packet_validation','group':group_index}))
    # AIRG v1: magic, version, count, stride, input words, shared words,
    # signed slot descriptors, shared raw words, then count strided snapshots.
    gather=(b'AIRG'+struct.pack('<IIIII',1,len(packets),stride,len(addresses),len(shared))
            +struct.pack('<'+'i'*len(descriptors),*descriptors)+b''.join(shared)+b''.join(source_rows))
    return packets,gather


def generate(replay, output, roles=('reconstruction_check','holdout'), packet_root=None, packet_group=None):
    calls = [call for call in replay['calls'] if call.get('role') in roles]
    if not calls or not any(call.get('role') == 'holdout' for call in calls):
        raise Unsupported('captured holdout call required')
    compiled = [_compile_call(call) for call in calls]
    records=[(shape,values,expected,{'call':call['call'],'role':call['role']}) for (shape,values,expected,_),call in zip(compiled,calls)]
    gather=None
    if packet_root is not None:
        records,gather=_packet_calls(packet_root,packet_group)
    if not records or not any(meta['role']=='holdout' for _,_,_,meta in records):
        raise Unsupported('holdout fixture required')
    shape = records[0][0]
    for other,values,expected,meta in records:
        if other != shape:
            raise Unsupported('calls have different expression topology')
        if _evaluate(shape,values) != expected:
            raise Unsupported('float32 reference differs from captured output in call ' + str(meta['call']))
    output.mkdir(parents=True,exist_ok=False)
    shader = _shader(shape)
    input_bytes = b''.join(b''.join(values) for _,values,_,_ in records)
    expected_bytes = b''.join(expected for _,_,expected,_ in records)
    (output/'generated.hlsl').write_text(shader,encoding='utf-8')
    (output/'input.bin').write_bytes(input_bytes)
    (output/'expected.bin').write_bytes(expected_bytes)
    if gather is not None:
        (output/'gather.bin').write_bytes(gather)
    contract = {
        'schema':1,'scope':'isolated captured expression graph; no live replacement or frame gain claim',
        'precision':'normal finite float32; round-to-nearest replay; byte equality required at GPU validation',
        'compiler_requirement':'D3DCOMPILE_IEEE_STRICTNESS; precise HLSL temporaries; no fast-math contraction',
        'count':len(records),'input_words_per_call':shape['inputs'],
        'output_words_per_call':len(shape['outputs']),
        'calls':[meta for _,_,_,meta in records],
        'operations':[{'op':op,'left':left,'right':right} for op,left,right in shape['operations']],
        'output_map':shape['outputs'],
        'input_sha256':hashlib.sha256(input_bytes).hexdigest(),
        'expected_sha256':hashlib.sha256(expected_bytes).hexdigest(),
        'shader_sha256':hashlib.sha256(shader.encode()).hexdigest(),
        'replacement_allowed':False,
        'live_blocker':('captured inputs change inside the callback and an early CPU consumer reads the result'
                        if packet_root is not None else 'live applicability, ownership, and consumer timing are not certified'),
        'timing_scope':'input.bin is packed offline; isolated harness full_ms starts after fixture creation and includes upload, dispatch, wait, and CPU readback consumption',
        'gather_scope':('gather.bin contains original-stride input_ready snapshots plus derived slot offsets and explicit shared words; harness gather mode times CPU packing in prep_ms'
                        if gather is not None else None),
    }
    (output/'contract.json').write_text(json.dumps(contract,indent=2)+'\n',encoding='utf-8')
    return contract


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('replay',type=Path,help='replay JSON or enclosing result JSON')
    parser.add_argument('output',type=Path)
    parser.add_argument('--packet-root',type=Path,help='captured batch directory with slice and batch folders')
    parser.add_argument('--packet-group',type=int,help='one captured group; final group is the heldout fixture')
    args=parser.parse_args()
    source=json.loads(args.replay.read_text(encoding='utf-8'))
    contract=generate(source.get('replay',source),args.output,packet_root=args.packet_root,packet_group=args.packet_group)
    print(json.dumps({'count':contract['count'],'input_words_per_call':contract['input_words_per_call'],'output_words_per_call':contract['output_words_per_call'],'replacement_allowed':False}))


if __name__=='__main__':main()
