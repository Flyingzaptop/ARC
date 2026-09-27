"""Lower a closed observed typed path to an isolated scratch-buffer GPU trial.

The shader never uses captured process addresses. It receives ordinal live-in
slots and writes validity, record bytes, and normalized cursor advance. This
module does not establish live binding, ownership, or a frame-time benefit.
"""
import argparse
import hashlib
import json
import shutil
import struct
import tempfile
from pathlib import Path

import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
from cpu_composite_shared import Unsupported, WIDTH, LEAVES
from cpu_composite_hlsl import (_word_expr, _byte_expr, _half_rne_bits,
                                _verify_half_nodes, _node_expr, _hlsl)
from cpu_composite_compaction import generate_compaction, compaction_reference






def _nodes(model):
    if model.get('status') != 'closed_observed_path':
        raise Unsupported('observed path is not closed')
    typed = model.get('typed_ir') or {}
    if typed.get('schema') != 1:
        raise Unsupported('typed graph schema is unsupported')
    nodes = typed.get('nodes') or []
    ids = [n.get('id') for n in nodes]
    if ids != list(range(len(nodes))) or any(n.get('type') not in WIDTH for n in nodes):
        raise Unsupported('typed node IDs or widths are invalid')
    for node in nodes:
        if len(bytes.fromhex(node['bytes'])) != WIDTH[node['type']]:
            raise Unsupported('typed node byte width mismatch')
    return typed, nodes


def _output_layout(typed, append):
    if append.get('status') != 'isolated_no_growth_sample' or not append.get('container', {}).get('cursor_is_end_pointer'):
        raise Unsupported('append layout is not a checked no-growth sample')
    stride = append.get('record_stride')
    if not isinstance(stride, int) or not 1 <= stride <= 16 or stride % 4:
        raise Unsupported('unsupported record stride')
    writes = typed.get('ordered_external_writes') or typed.get('outputs') or []
    footprint = append.get('ordered_writes') or []
    if len(writes) != len(footprint):
        raise Unsupported('external write footprint differs from append contract')
    base = None
    header = []
    layout = []
    for write, place in zip(writes, footprint):
        width = write.get('width')
        if width != place.get('width') or len(bytes.fromhex(write['bytes'])) != width:
            raise Unsupported('external write width differs from append contract')
        if place.get('target') == 'cursor_header':
            if width != 8:
                raise Unsupported('cursor metadata width')
            header.append(write)
            layout.append({'kind': 'cursor_delta', 'source': write['source'], 'width': width})
        else:
            offset = place.get('offset')
            if not isinstance(offset, int) or offset < 0 or offset + width > stride:
                raise Unsupported('record write outside scratch stride')
            candidate = write['address'] - offset
            if base is None:
                base = candidate
            elif base != candidate:
                raise Unsupported('record writes do not share a captured base')
            layout.append({'kind': 'record', 'offset': offset, 'width': width,
                           'source': write['source']})
    if base is None or len(header) != 1:
        raise Unsupported('record or cursor output is missing')
    cursor_after = int.from_bytes(bytes.fromhex(header[0]['bytes']), 'little')
    if cursor_after - base != stride:
        raise Unsupported('captured cursor advance differs from record stride')
    # Every output must have a typed source with the exact observed low bytes.
    nodes = {node['id']: node for node in typed['nodes']}
    for write in writes:
        source = nodes.get(write.get('source'))
        if source is None or bytes.fromhex(source['bytes'])[:write['width']] != bytes.fromhex(write['bytes']):
            raise Unsupported('typed output source does not reproduce captured bytes')
    final_spans = typed.get('outputs') or []
    if final_spans is not writes:
        final_bytes = {}
        for write in writes:
            raw = bytes.fromhex(write['bytes'])
            for j, value in enumerate(raw):
                final_bytes[write['address'] + j] = value
        for span in final_spans:
            raw = bytes.fromhex(span['bytes'])
            if len(raw) != span['width'] or any(final_bytes.get(span['address'] + j) != value
                                                   for j, value in enumerate(raw)):
                raise Unsupported('final output spans disagree with ordered writes')
    return layout, stride, base


def _reachable(nodes, layout, guards):
    required = {item['source'] for item in layout if item['kind'] == 'record'}
    for guard in guards:
        if guard['predicate'] in ('jmp', 'ret', 'call'):
            continue
        if guard['predicate'] not in ('je', 'jne', 'jbe', 'ja') or not isinstance(guard.get('source'), int):
            raise Unsupported('branch guard cannot be lowered')
        required.add(guard['source'])
    seen = set()
    def visit(i):
        if i in seen:
            return
        if not isinstance(i, int) or not 0 <= i < len(nodes):
            raise Unsupported('invalid DAG reference')
        node = nodes[i]
        for source in node.get('inputs', []):
            visit(source['id'] if isinstance(source, dict) else source)
        seen.add(i)
    for i in required:
        visit(i)
    return sorted(seen)
















def generate(models, append_contract, output, packet_preflight=None, *, allow_unbounded_sources=False):
    if not isinstance(models, list):
        models = [models]
    if not models or len(models) > 65536:
        raise Unsupported('packet count outside budget')
    typed, nodes = _nodes(models[0])
    _verify_half_nodes(nodes)
    layout, stride, first_base = _output_layout(typed, append_contract)
    skipped_guard_sources, packet_scope = _packet_guard_filter(
        models, append_contract, packet_preflight,
        allow_unbounded_sources=allow_unbounded_sources)
    if packet_preflight is not None and first_base != packet_preflight['cursor']:
        raise Unsupported('packet preflight cursor differs from first observed record base')
    guards = [g for g in typed.get('guards', []) if skipped_guard_sources is None or
              g.get('source') not in skipped_guard_sources]
    reachable = _reachable(nodes, layout, guards)
    leaf_ids = [i for i in reachable if nodes[i]['op'] in LEAVES]
    slots, words = {}, 0
    for i in leaf_ids:
        slots[i] = words
        words += (WIDTH[nodes[i]['type']] + 3) // 4
    if not words or words > 64 or 1 + stride // 4 + 2 > 64:
        raise Unsupported('isolated benchmark word budget')
    shader = _hlsl(nodes, reachable, slots, layout, guards, stride, words)
    fixture_inputs = []
    fixture_expected = []
    def shape(n):
        return (n['op'], n['type'], n.get('inputs', []), n.get('byte_offset'),
                n.get('width'), n.get('control'), n.get('mask'), n.get('rounding'),
                n.get('reduction'), n.get('value'),
                n['bytes'] if n['op'] in ('constant', 'immediate') else None)
    signature = [shape(n) for n in nodes]
    layout_shape = [(item['kind'], item.get('offset'), item['width'], item['source']) for item in layout]
    guard_shape = [(g.get('predicate'), g.get('taken'), g.get('source')) for g in guards]
    for row_index, model in enumerate(models):
        current, row_nodes = _nodes(model)
        _verify_half_nodes(row_nodes)
        current_layout, current_stride, current_base = _output_layout(current, append_contract)
        if packet_preflight is not None and current_base != first_base + row_index * stride:
            raise Unsupported('observed positive packet cursor progression differs from normalized stride')
        if ([shape(n) for n in row_nodes] != signature
                or current_stride != stride or
                [(item['kind'], item.get('offset'), item['width'], item['source']) for item in current_layout] != layout_shape or
                [(g.get('predicate'), g.get('taken'), g.get('source')) for g in current.get('guards', [])
                 if skipped_guard_sources is None or g.get('source') not in skipped_guard_sources] != guard_shape):
            raise Unsupported('packet rows have different topology, outputs, or guard path')
        row_input = bytearray()
        for i in leaf_ids:
            raw = bytes.fromhex(row_nodes[i]['bytes'])
            row_input.extend(raw.ljust(((len(raw) + 3) // 4) * 4, b'\0'))
        fixture_inputs.extend(row_input)
        record = bytearray(stride)
        ordered = current.get('ordered_external_writes') or current.get('outputs') or []
        for item, write in zip(current_layout, ordered):
            if item['kind'] == 'record':
                record[item['offset']:item['offset'] + item['width']] = bytes.fromhex(write['bytes'])
        fixture_expected.extend(struct.pack('<I', 1) + record + struct.pack('<Q', stride))
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    (output/'generated.hlsl').write_bytes(shader.encode('utf-8'))
    (output/'input.bin').write_bytes(bytes(fixture_inputs))
    (output/'expected.bin').write_bytes(bytes(fixture_expected))
    contract = {'schema': 1, 'scope': 'isolated scratch reconstruction of observed path',
                'count': len(models), 'input_words_per_call': words,
                'output_words_per_call': 1 + stride // 4 + 2,
                'record_stride': stride, 'scratch_layout':
                {'validity_word': 0, 'record_start_word': 1, 'cursor_delta_start_word': 1 + stride // 4},
                'inputs': [{'node': i, 'slot': slots[i], 'type': nodes[i]['type'],
                            'origin': nodes[i]['op']} for i in leaf_ids],
                'ordered_output_writes': layout,
                'typed_ir_missing_live_contract': typed.get('missing', []),
                'packet_scope': packet_scope,
                'pruned_pointer_nodes': [item['source'] for item in layout if item['kind'] == 'cursor_delta'],
                'reachable_nodes': reachable,
                'shader_sha256': hashlib.sha256((output/'generated.hlsl').read_bytes()).hexdigest(),
                'input_sha256': hashlib.sha256(bytes(fixture_inputs)).hexdigest(),
                'expected_sha256': hashlib.sha256(bytes(fixture_expected)).hexdigest(),
                'replacement_allowed': False,
                'full_batch_replacement_allowed': False,
                'batch_compaction': 'not_generated; per-row validity is scratch evidence for future prefix/scatter',
                'packet_preparation': 'captured leaf values packed on CPU for isolated trial',
                'limitations': ['entry register/vector binding is captured only',
                                'one observed path and no-growth append layout only',
                                'no single initial cursor/capacity preflight, GPU prefix, scatter, or final count publication',
                                'f32tof16 and all output bytes require independent GPU validation',
                                'ownership, alternate paths, consumer timing, and full-frame economics unproven']}
    (output/'contract.json').write_text(json.dumps(contract, indent=2) + '\n', encoding='utf-8')
    return contract











def _packet_guard_filter(*args, **kwargs):
    from cpu_composite_bulk import _packet_guard_filter as impl
    return impl(*args, **kwargs)

def generate_packet_shader_plan(*args, **kwargs):
    from cpu_composite_bulk import generate_packet_shader_plan as impl
    return impl(*args, **kwargs)

def generate_bulk_fixture(*args, **kwargs):
    from cpu_composite_bulk import generate_bulk_fixture as impl
    return impl(*args, **kwargs)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    ir_source = parser.add_mutually_exclusive_group(required=True)
    ir_source.add_argument('--ir', type=Path, action='append', help='one or more saved typed path JSON files')
    ir_source.add_argument('--capture', type=Path, help='captured traversal directory for analyze_paths')
    append_source = parser.add_mutually_exclusive_group(required=True)
    append_source.add_argument('--append-contract', type=Path)
    append_source.add_argument('--append-capture', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--emit-compaction', type=Path, help='also generate generic GPU prefix/scatter shaders here')
    parser.add_argument('--bulk-proof', type=Path, help='checked counted-tail and packet preflight JSON')
    parser.add_argument('--packet-shader-plan', action='store_true',
                        help='emit HLSL/slot contract only while bulk source binding is incomplete')
    parser.add_argument('--bulk-input', type=Path, help='snapshot-gathered packed ordinal inputs')
    parser.add_argument('--bulk-expected-records', type=Path, help='independent after-view record bytes')
    args = parser.parse_args()
    if args.capture:
        from cpu_composite_ir import analyze_paths
        models, _ = analyze_paths(args.capture)
    else:
        models = [json.loads(path.read_text()) for path in args.ir]
    if args.append_capture:
        from cpu_append_contract import derive_append_contract
        append = derive_append_contract(args.append_capture)
    else:
        append = json.loads(args.append_contract.read_text())
    if args.bulk_proof:
        proof = json.loads(args.bulk_proof.read_text())
        if args.packet_shader_plan:
            if args.bulk_input or args.bulk_expected_records:
                parser.error('static packet plan cannot include bound input files')
            result = generate_packet_shader_plan(models, append, proof, args.out)
        elif not args.bulk_input or not args.bulk_expected_records:
            parser.error('--bulk-proof requires --bulk-input and --bulk-expected-records')
        else:
            result = generate_bulk_fixture(models, append, proof,
                                           args.bulk_input, args.bulk_expected_records, args.out)
    else:
        if args.bulk_input or args.bulk_expected_records or args.packet_shader_plan:
            parser.error('bulk inputs and packet shader plan require --bulk-proof')
        result = generate(models, append, args.out)
    if args.emit_compaction:
        generate_compaction(args.emit_compaction)
    print(json.dumps({'count': result['count'], 'input_words_per_call': result['input_words_per_call'],
                      'output_words_per_call': result['output_words_per_call'], 'replacement_allowed': False}))


if __name__ == '__main__':
    main()
