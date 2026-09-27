"""Typed scalar/vector HLSL lowering and exact half conversion checks."""
import struct
from cpu_composite_shared import Unsupported, WIDTH, LEAVES

def _word_expr(node, ref, byte_offset):
    typ = node['type']
    if byte_offset < 0 or byte_offset >= WIDTH[typ]:
        raise Unsupported('byte extraction outside source')
    if typ in ('u32x4', 'f32x4'):
        word = f'v{ref}[{byte_offset // 4}]'
        if typ == 'f32x4':
            word = f'asuint({word})'
    elif typ == 'u64':
        word = f'v{ref}.{ "x" if byte_offset < 4 else "y" }'
    elif typ == 'f32':
        word = f'asuint(v{ref})'
    else:
        word = f'v{ref}'
    return word, byte_offset % 4


def _byte_expr(nodes, ref, offset):
    word, shift = _word_expr(nodes[ref], ref, offset)
    return f'(({word} >> {shift * 8}) & 255u)'


def _half_rne_bits(bits):
    """IEEE binary32 bits to binary16 bits, nearest even (value only)."""
    sign = (bits >> 16) & 0x8000
    exponent = (bits >> 23) & 255
    mantissa = bits & 0x7fffff
    if exponent == 255:
        return sign | (0x7c00 if mantissa == 0 else 0x7e00 | (mantissa >> 13))
    e = exponent - 127
    if e > 15:
        return sign | 0x7c00
    if e >= -14:
        rounded = mantissa + 0xfff + ((mantissa >> 13) & 1)
        return (sign | (((e + 15) << 10) + (rounded >> 13))) & 0xffff
    if e < -25:
        return sign
    shift = -e - 1
    significant = mantissa | 0x800000
    quotient = significant >> shift
    remainder = significant & ((1 << shift) - 1)
    halfway = 1 << (shift - 1)
    return sign | (quotient + int(remainder > halfway or
                                   (remainder == halfway and quotient & 1)))


def _verify_half_nodes(nodes):
    for node in nodes:
        if node['op'] != 'vcvtps2ph':
            continue
        refs = node.get('inputs', [])
        if len(refs) != 1 or not isinstance(refs[0], int):
            raise Unsupported('half conversion source')
        source = bytes.fromhex(nodes[refs[0]]['bytes'])
        if len(source) != 16:
            raise Unsupported('half conversion source width')
        actual = b''.join(struct.pack('<H', _half_rne_bits(struct.unpack_from('<I', source, j)[0]))
                          for j in (0, 4, 8, 12)) + bytes(8)
        if actual != bytes.fromhex(node['bytes']):
            raise Unsupported('half conversion differs from captured CPU value')


def _node_expr(node, nodes, slot):
    typ, op, refs = node['type'], node['op'], node.get('inputs', [])
    if op in LEAVES:
        if slot is None:
            raise Unsupported('live-in slot not assigned')
        at = slot * 4
        if typ == 'u64':
            return f'uint2(Inputs.Load((i * InputWords + {slot}) * 4), Inputs.Load((i * InputWords + {slot + 1}) * 4))'
        if typ == 'u32x4':
            return f'Inputs.Load4((i * InputWords + {slot}) * 4)'
        if typ == 'f32x4':
            return f'asfloat(Inputs.Load4((i * InputWords + {slot}) * 4))'
        word = f'Inputs.Load((i * InputWords + {slot}) * 4)'
        if typ == 'f32':
            return f'asfloat({word})'
        return f'({word} & {((1 << (8 * WIDTH[typ])) - 1)}u)' if WIDTH[typ] < 4 else word
    if op in ('immediate', 'constant'):
        if typ not in ('u8', 'u16', 'u32', 'u64'):
            raise Unsupported('immediate width')
        value = int.from_bytes(bytes.fromhex(node['bytes']), 'little')
        if op == 'immediate' and (node['value'] & ((1 << (8 * WIDTH[typ])) - 1)) != value:
            raise Unsupported('immediate value differs from captured bytes')
        return (f'uint2({value & 0xffffffff}u, {value >> 32}u)' if typ == 'u64'
                else f'{value}u')
    def arg(index):
        value = refs[index]
        if not isinstance(value, int):
            raise Unsupported('non-scalar DAG input')
        return f'v{value}'
    def as_u4(ref):
        return f'asuint(v{ref})' if nodes[ref]['type'] == 'f32x4' else f'v{ref}'
    def as_f4(ref):
        return f'asfloat(v{ref})' if nodes[ref]['type'] == 'u32x4' else f'v{ref}'
    def as_f(ref):
        return f'asfloat(v{ref})' if nodes[ref]['type'] in ('u8', 'u16', 'u32') else f'v{ref}'
    if op in ('vpxor', 'vxorps') and typ == 'u32x4' and len(refs) == 2:
        return f'({as_u4(refs[0])} ^ {as_u4(refs[1])})'
    if op in ('vaddss', 'vsubss') and typ == 'f32x4' and len(refs) == 2:
        left, right = as_f4(refs[0]), as_f(refs[1])
        return f'float4(({left}).x {"+" if op == "vaddss" else "-"} {right}, ({left}).yzw)'
    if op == 'vsubps' and typ == 'f32x4' and len(refs) == 2:
        return f'({as_f4(refs[0])} - {as_f4(refs[1])})'
    if op == 'vsqrtps' and typ == 'f32x4' and len(refs) == 1:
        return f'sqrt({as_f4(refs[0])})'
    if op == 'vinsertps' and typ == 'f32x4' and len(refs) == 2:
        control = node.get('control')
        if not isinstance(control, int) or not 0 <= control <= 255:
            raise Unsupported('insert control')
        base = as_f4(refs[0]); src = as_f4(refs[1]) if nodes[refs[1]]['type'] in ('f32x4', 'u32x4') else as_f(refs[1])
        src_lane = (control >> 6) & 3 if nodes[refs[1]]['type'] in ('f32x4', 'u32x4') else 0
        values = [f'({base}).{c}' for c in 'xyzw']
        values[(control >> 4) & 3] = f'({src}).{ "xyzw"[src_lane] }' if nodes[refs[1]]['type'] in ('f32x4', 'u32x4') else src
        for lane in range(4):
            if control & (1 << lane):
                values[lane] = '0.0f'
        return 'float4(' + ', '.join(values) + ')'
    if op == 'vdpps' and typ == 'f32x4' and len(refs) == 2:
        mask = node.get('mask')
        if not isinstance(mask, int) or not 0 <= mask <= 255 or node.get('reduction') != 'pairwise_float32':
            raise Unsupported('dot product mode')
        a, b = as_f4(refs[0]), as_f4(refs[1])
        terms = [f'(({a}).{c} * ({b}).{c})' if mask & (1 << (lane + 4)) else '0.0f'
                 for lane, c in enumerate('xyzw')]
        dot = f'(({terms[0]} + {terms[1]}) + ({terms[2]} + {terms[3]}))'
        return 'float4(' + ', '.join(dot if mask & (1 << lane) else '0.0f' for lane in range(4)) + ')'
    if op == 'vcomiss' and typ == 'bytes3' and len(refs) == 2:
        a, b = as_f(refs[0]), as_f(refs[1])
        unordered = f'(isnan({a}) || isnan({b}))'
        return f'uint3(({unordered} || {a} == {b}) ? 1u : 0u, ({unordered} || {a} < {b}) ? 1u : 0u, {unordered} ? 1u : 0u)'
    if op in ('add', 'sub', 'and', 'or', 'xor', 'shl', 'shr', 'mul') and len(refs) == 2:
        if typ == 'u64':
            if op not in ('add', 'sub', 'and', 'or', 'xor'):
                raise Unsupported('u64 operation ' + op)
            if op in ('add', 'sub'):
                return f'{op}64({arg(0)}, {arg(1)})'
            return f'({arg(0)} { {"and":"&","or":"|","xor":"^"}[op] } {arg(1)})'
        if typ not in ('u8', 'u16', 'u32', 'f32'):
            raise Unsupported('arithmetic type ' + typ)
        symbols = {'add': '+', 'sub': '-', 'and': '&', 'or': '|', 'xor': '^',
                   'shl': '<<', 'shr': '>>', 'mul': '*'}
        if typ == 'f32' and op not in ('add', 'sub', 'mul'):
            raise Unsupported('float bitwise operation')
        expr = f'({arg(0)} {symbols[op]} {arg(1)})'
        return f'({expr} & {(1 << (8 * WIDTH[typ])) - 1}u)' if typ in ('u8', 'u16') else expr
    if op in ('cmp', 'test') and len(refs) == 2:
        if typ == 'u64':
            return f'sub64({arg(0)}, {arg(1)})' if op == 'cmp' else f'({arg(0)} & {arg(1)})'
        if typ not in ('u8', 'u16', 'u32'):
            raise Unsupported('integer flag width')
        symbol = '-' if op == 'cmp' else '&'
        return f'(({arg(0)} {symbol} {arg(1)}) & {(1 << (8 * WIDTH[typ])) - 1}u)'
    if op in ('extract_bytes', 'extract_lane') and len(refs) == 1:
        source = refs[0]
        if not isinstance(source, int):
            raise Unsupported('extract input')
        offset = node.get('byte_offset', node.get('lane', 0) * 4)
        width = node.get('width', WIDTH[typ])
        if width != WIDTH[typ] or width > 4 or offset // 4 != (offset + width - 1) // 4:
            raise Unsupported('cross-word byte extraction')
        word, shift = _word_expr(nodes[source], source, offset)
        mask = (1 << (8 * width)) - 1
        return f'(({word} >> {shift * 8}) & {mask}u)'
    if op == 'pack_bytes' and typ == 'u32x4' and len(refs) == 16:
        lanes = []
        for lane in range(4):
            parts = []
            for byte in range(4):
                ref = refs[lane * 4 + byte]
                if not isinstance(ref, dict):
                    raise Unsupported('pack byte reference')
                parts.append(f'({_byte_expr(nodes, ref["id"], ref["byte_offset"])} << {byte * 8})')
            lanes.append(' | '.join(parts))
        return 'uint4(' + ', '.join(lanes) + ')'
    if op == 'vcvtps2ph' and typ == 'u32x4' and len(refs) == 1:
        source = arg(0)
        return ('uint4(half_rne_bits(' + source + '.x) | (half_rne_bits(' + source + '.y) << 16), '
                'half_rne_bits(' + source + '.z) | (half_rne_bits(' + source + '.w) << 16), 0u, 0u)')
    raise Unsupported('typed operation outside lowering class: ' + op + ' ' + typ)


def _hlsl(nodes, reachable, slots, layout, guards, stride, input_words):
    types = {'u8': 'uint', 'u16': 'uint', 'u32': 'uint', 'u64': 'uint2',
             'f32': 'float', 'u32x4': 'uint4', 'f32x4': 'float4', 'bytes3': 'uint3'}
    lines = [
        '// Generated from typed observed provenance; isolated scratch output only.',
        'ByteAddressBuffer Inputs : register(t0);',
        'RWByteAddressBuffer Outputs : register(u0);',
        'cbuffer Parameters : register(b0) { uint Count; };',
        f'static const uint InputWords = {input_words}u;',
        f'static const uint OutputWords = {1 + stride // 4 + 2}u;',
        'uint2 add64(uint2 a, uint2 b) { uint lo=a.x+b.x; return uint2(lo,a.y+b.y+(lo<a.x)); }',
        'uint2 sub64(uint2 a, uint2 b) { return uint2(a.x-b.x,a.y-b.y-(a.x<b.x)); }',
        'uint half_rne_bits(uint bits) {',
        '  uint sign=(bits >> 16) & 0x8000u;',
        '  uint exponent=(bits >> 23) & 255u;',
        '  uint mantissa=bits & 0x7fffffu;',
        '  if (exponent==255u) return sign | (mantissa==0u ? 0x7c00u : (0x7e00u | (mantissa >> 13)));',
        '  int e=int(exponent)-127;',
        '  if (e>15) return sign | 0x7c00u;',
        '  if (e>=-14) { uint rounded=mantissa+0xfffu+((mantissa >> 13)&1u);',
        '    return sign | ((uint(e+15) << 10)+(rounded >> 13)); }',
        '  if (e < -25) return sign;',
        '  uint shift=uint(-e-1); uint significant=mantissa | 0x800000u;',
        '  uint quotient=significant >> shift;',
        '  uint remainder=significant & ((1u << shift)-1u);',
        '  uint halfway=1u << (shift-1u);',
        '  return sign | (quotient + uint(remainder>halfway || (remainder==halfway && (quotient&1u)!=0u)));',
        '}',
        '[numthreads(64,1,1)]',
        'void main(uint3 id : SV_DispatchThreadID) {',
        '  uint i=id.x; if (i>=Count) return;',
    ]
    for index in reachable:
        node = nodes[index]
        qualifier = 'precise ' if node['type'] in ('f32', 'f32x4') else ''
        lines.append(f'  {qualifier}{types[node["type"]]} v{index} = {_node_expr(node, nodes, slots.get(index))};')
    tests = []
    for guard in guards:
        predicate = guard['predicate']
        if predicate in ('jmp', 'ret', 'call'):
            continue
        if predicate not in ('je', 'jne', 'jbe', 'ja'):
            raise Unsupported('guard predicate outside lowering class')
        source = guard['source']
        if predicate in ('ja', 'jbe'):
            if nodes[source]['op'] != 'vcomiss' or nodes[source]['type'] != 'bytes3':
                raise Unsupported('ordered float guard source')
            above = f'((v{source}.x | v{source}.y) == 0u)'
            taken = above if predicate == 'ja' else f'(!{above})'
        else:
            typ = nodes[source]['type']
            if typ == 'u64':
                zero = f'((v{source}.x | v{source}.y) == 0u)'
            elif typ in ('u8', 'u16', 'u32'):
                zero = f'(v{source} == 0u)'
            elif typ == 'bytes3':
                zero = f'(v{source}.x != 0u)'
            else:
                raise Unsupported('zero flag source type')
            taken = zero if predicate == 'je' else f'(!{zero})'
        tests.append(taken if guard['taken'] else f'(!{taken})')
    lines.append('  uint valid = ' + (' && '.join(tests) if tests else 'true') + ' ? 1u : 0u;')
    lines.append('  uint4 record = uint4(0u,0u,0u,0u);')
    for item in layout:
        if item['kind'] != 'record':
            continue
        for j in range(item['width']):
            offset = item['offset'] + j
            lane, shift = divmod(offset, 4)
            shift *= 8
            byte = _byte_expr(nodes, item['source'], j)
            lines.append(f'  record[{lane}] = (record[{lane}] & ~(255u << {shift})) | ({byte} << {shift});')
    lines.append('  uint outBase=(i*OutputWords)*4;')
    lines.append('  Outputs.Store(outBase, valid);')
    for lane in range(stride // 4):
        lines.append(f'  Outputs.Store(outBase + {(1 + lane) * 4}, valid != 0u ? record[{lane}] : 0u);')
    lines.append(f'  Outputs.Store(outBase + {(1 + stride // 4) * 4}, valid != 0u ? {stride}u : 0u);')
    lines.append(f'  Outputs.Store(outBase + {(2 + stride // 4) * 4}, 0u);')
    lines.append('}')
    return '\n'.join(lines) + '\n'
