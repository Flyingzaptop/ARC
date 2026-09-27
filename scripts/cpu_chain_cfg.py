"""Bounded, engine-name-blind CFG/effect survey of captured x86-64 code.

This is a discovery aid, not a proof of an executable replacement. Addresses in
the output are RVAs; capture events supply the ASLR base and starting points.
"""
import argparse
import hashlib
import json
import struct
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path

import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM

from cpu_producer_plan import memory as producer_memory


@dataclass(frozen=True)
class Budgets:
    max_functions: int = 24
    max_depth: int = 3
    max_instructions: int = 4096
    max_edges: int = 8192
    max_segment_bytes: int = 32 * 1024 * 1024


class CodeImage:
    def __init__(self, base, segments):
        self.base = base
        # A narrow live capture takes precedence over an overlapping PE section.
        self.segments = sorted(segments, key=lambda item: len(item[1]))

    def read(self, va, size=15):
        for start, data, label in self.segments:
            if start <= va < start + len(data):
                offset = va - start
                return data[offset:offset + size], label
        return b'', None

    def contains(self, va):
        return bool(self.read(va, 1)[0])

    def rva(self, va):
        return va - self.base


def executable_pe(path, base, max_bytes):
    """Map only executable PE sections, using the observed module base."""
    blob = Path(path).read_bytes()
    if blob[:2] != b'MZ' or len(blob) < 0x40:
        raise ValueError('not a PE image')
    pe = struct.unpack_from('<I', blob, 0x3c)[0]
    if blob[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE image')
    count, opt_size = struct.unpack_from('<H', blob, pe + 6)[0], struct.unpack_from('<H', blob, pe + 20)[0]
    sections = []
    for i in range(count):
        off = pe + 24 + opt_size + i * 40
        if off + 40 > len(blob):
            raise ValueError('truncated PE section table')
        name = blob[off:off + 8].split(b'\0')[0].decode('ascii', 'replace')
        virtual_size, rva, raw_size, raw_off = struct.unpack_from('<IIII', blob, off + 8)
        flags = struct.unpack_from('<I', blob, off + 36)[0]
        if flags & 0x20000000 and raw_size:
            if raw_size > max_bytes or raw_off + raw_size > len(blob):
                raise ValueError('executable section exceeds capture budget')
            data = blob[raw_off:raw_off + min(raw_size, virtual_size or raw_size)]
            sections.append((base + rva, data, 'pe:' + name))
    if not sections:
        raise ValueError('no executable PE sections')
    return sections


def _image_base(path):
    with Path(path).open('rb') as source:
        header = source.read(4096)
    pe = struct.unpack_from('<I', header, 0x3c)[0]
    optional = pe + 24
    magic = struct.unpack_from('<H', header, optional)[0]
    if magic == 0x20b:
        return struct.unpack_from('<Q', header, optional + 24)[0]
    if magic == 0x10b:
        return struct.unpack_from('<I', header, optional + 28)[0]
    raise ValueError('unsupported PE optional header')


def _sha256(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def _jsonl(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def load_capture(root, image=None, budgets=Budgets(), selection=None):
    root = Path(root)
    observations, segments, seeds = [], [], set()
    base = None
    for folder in (root / 'watch', root / 'upstream', root / 'adjacent'):
        events_path = folder / 'writes.jsonl'
        if not events_path.is_file():
            continue
        events = _jsonl(events_path)
        for e in events:
            if 'module_base' not in e or 'function_begin' not in e:
                continue
            if base is None:
                base = e['module_base']
            elif base != e['module_base']:
                raise ValueError('mixed module bases')
            observations.append(dict(e, source=folder.name))
            seeds.add(e['function_begin'])
            path = folder / ('function-%d.bin' % e['event'])
            if path.is_file() and path.stat().st_size <= budgets.max_segment_bytes:
                segments.append((e['function_begin'], path.read_bytes(), str(path.relative_to(root))))
            caller_path = folder / ('callers-%d.json' % e['event'])
            if caller_path.is_file():
                for depth, caller in enumerate(json.loads(caller_path.read_text())):
                    binary = folder / ('caller-%d-%d.bin' % (e['event'], depth))
                    if binary.is_file() and binary.stat().st_size <= budgets.max_segment_bytes:
                        segments.append((caller['begin'], binary.read_bytes(), str(binary.relative_to(root))))
                        seeds.add(caller['begin'])
    selected = json.loads(Path(selection).read_text()) if selection else None
    selected_seeds = []
    if selected and not image:
        raise ValueError('selection requires its captured PE image')
    if base is None:
        if not selected:
            raise ValueError('no captured producer observations')
        base = _image_base(image)
    identity = {'image_sha256': None, 'verification': 'captured_code_only'}
    if image:
        digest = _sha256(image)
        expected = []
        if selected:
            expected.append(selected['image_sha256'])
        for metadata in (root / 'candidates.json', root / 'probe' / 'candidates.json',
                         root / 'selection.json'):
            if metadata.is_file():
                document = json.loads(metadata.read_text())
                for key in ('module_sha256', 'image_sha256'):
                    if key in document:
                        expected.append(document[key])
        if any(value.lower() != digest for value in expected):
            raise ValueError('captured image SHA does not match evidence or selection')
        pe_sections = executable_pe(image, base, budgets.max_segment_bytes)
        def pe_bytes(start, length):
            for section_start, data, _ in pe_sections:
                if section_start <= start and start + length <= section_start + len(data):
                    return data[start - section_start:start - section_start + length]
            return None
        if segments:
            for start, data, label in segments:
                if pe_bytes(start, len(data)) != data:
                    raise ValueError('captured code differs from PE image: ' + label)
        elif not expected:
            raise ValueError('PE image has neither matching SHA evidence nor captured code to verify')
        identity = {'image_sha256': digest,
                    'verification': 'sha256_and_captured_code' if expected and segments else
                                    'sha256' if expected else 'captured_code_bytes',
                    'matched_capture_segments': len(segments)}
        segments.extend(pe_sections)
        if selected:
            for entry in selected['candidates']:
                rva = entry['entry_rva']
                if not isinstance(rva, int) or rva < 0 or pe_bytes(base + rva, 1) is None:
                    raise ValueError('selection entry outside verified executable image')
                seeds.add(base + rva)
                selected_seeds.append(base + rva)
    # Prefer the largest capture at a given start and keep a single copy.
    distinct = {}
    for start, data, label in segments:
        if start not in distinct or len(data) > len(distinct[start][0]):
            distinct[start] = (data, label)
    prioritized = list(dict.fromkeys(selected_seeds))
    prioritized.extend(sorted(seeds - set(prioritized)))
    return CodeImage(base, [(start, *value) for start, value in distinct.items()]), prioritized, observations, identity


def _memory_effects(ins):
    effects = []
    for operand in ins.operands:
        if operand.type != X86_OP_MEM or ins.mnemonic == 'lea':
            continue
        try:
            m = producer_memory(ins, operand)
        except (ValueError, IndexError):
            effects.append({'kind': 'unknown', 'reason': 'unsupported memory address'})
            continue
        access = operand.access
        read = bool(access & capstone.CS_AC_READ)
        write = bool(access & capstone.CS_AC_WRITE) or m['write']
        if not read and not write:
            effects.append({'kind': 'unknown', 'address': m, 'reason': 'operand access not decoded'})
        else:
            effects.append({'kind': 'read_write' if read and write else 'write' if write else 'read', 'address': m})
    return effects


def analyze_graph(image, seeds, budgets=Budgets()):
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    functions, nodes, edges, limitations = [], {}, [], []
    pending = deque((seed, 0, 'observed_function') for seed in seeds)
    visited = set()

    def limitation(kind, pc, detail):
        item = {'kind': kind, 'at_rva': image.rva(pc), 'detail': detail,
                'function_entry_rva': image.rva(entry)}
        if item not in limitations:
            limitations.append(item)

    while pending:
        entry, depth, reason = pending.popleft()
        if entry in visited:
            continue
        if len(visited) >= budgets.max_functions:
            limitation('function_budget', entry, 'function traversal stopped')
            break
        if not image.contains(entry):
            limitation('missing_code', entry, reason)
            continue
        visited.add(entry)
        function = {'entry_rva': image.rva(entry), 'depth': depth, 'discovery': reason,
                    'nodes': 0, 'node_rvas': []}
        functions.append(function)
        work = deque([entry])
        local = set()
        while work:
            pc = work.popleft()
            if pc in local:
                continue
            if len(nodes) >= budgets.max_instructions or len(edges) >= budgets.max_edges:
                limitation('graph_budget', pc, 'instruction or edge cap reached')
                work.clear()
                break
            code, label = image.read(pc)
            if not code:
                limitation('missing_code', pc, 'control-flow successor outside captured code')
                continue
            ins = next(decoder.disasm(code, pc, count=1), None)
            if ins is None:
                limitation('decode_failure', pc, label)
                continue
            local.add(pc)
            function['nodes'] += 1
            function['node_rvas'].append(image.rva(pc))
            next_pc = pc + ins.size
            node = {'rva': image.rva(pc), 'size': ins.size, 'code': ins.bytes.hex(),
                    'mnemonic': ins.mnemonic, 'operands': ins.op_str,
                    'memory': _memory_effects(ins), 'capture': label}
            if pc in nodes:
                node = nodes[pc]
                if image.rva(entry) not in node['function_entries']:
                    node['function_entries'].append(image.rva(entry))
            else:
                node['function_entries'] = [image.rva(entry)]
                nodes[pc] = node
            if capstone.x86_const.X86_PREFIX_LOCK in ins.prefix or ins.mnemonic in (
                    'cmpxchg', 'cmpxchg8b', 'cmpxchg16b', 'xadd') or (
                    ins.mnemonic == 'xchg' and node['memory']):
                limitation('unsupported_atomic_path', pc, ins.mnemonic)

            def follow(target, kind):
                edges.append({'from_rva': image.rva(pc), 'to_rva': image.rva(target), 'kind': kind})
                if image.contains(target):
                    work.append(target)
                else:
                    limitation('missing_code', target, kind)

            if ins.group(capstone.CS_GRP_RET):
                continue
            if ins.group(capstone.CS_GRP_CALL):
                target = ins.operands[0].imm if ins.operands and ins.operands[0].type == X86_OP_IMM else None
                if target is None:
                    limitation('indirect_call_effects_unknown', pc, ins.op_str)
                elif not image.contains(target):
                    limitation('external_or_uncaptured_call_effects_unknown', pc, hex(image.rva(target)))
                elif depth >= budgets.max_depth:
                    limitation('call_depth_budget', pc, hex(image.rva(target)))
                else:
                    edges.append({'from_rva': image.rva(pc), 'to_rva': image.rva(target), 'kind': 'call'})
                    pending.append((target, depth + 1, 'direct_call'))
                follow(next_pc, 'call_return_assumed')
            elif ins.group(capstone.CS_GRP_JUMP):
                target = ins.operands[0].imm if ins.operands and ins.operands[0].type == X86_OP_IMM else None
                if target is None:
                    limitation('indirect_branch_unknown', pc, ins.op_str)
                else:
                    follow(target, 'jump' if ins.mnemonic == 'jmp' else 'branch_taken')
                if ins.mnemonic != 'jmp':
                    follow(next_pc, 'branch_fallthrough')
            elif ins.group(capstone.CS_GRP_IRET) or ins.mnemonic in ('ud2', 'int3'):
                limitation('terminating_instruction', pc, ins.mnemonic)
            else:
                follow(next_pc, 'fallthrough')
    for function in functions:
        function['node_rvas'].sort()
    return {'functions': functions, 'nodes': sorted(nodes.values(), key=lambda n: n['rva']),
            'edges': edges, 'limitations': limitations,
            'budgets': vars(budgets)}


def ordered_writers(observations, image, nodes):
    by_target = defaultdict(list)
    end_to_pc = {n['rva'] + n['size']: n['rva'] for n in nodes}
    by_pc = {n['rva']: n for n in nodes}
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    def immediate_store(pc):
        if pc is None:
            return False
        node = by_pc[pc]
        ins = next(decoder.disasm(bytes.fromhex(node['code']), image.base + pc), None)
        return bool(ins and ins.mnemonic == 'mov' and len(ins.operands) == 2 and
                    ins.operands[0].type == X86_OP_MEM and ins.operands[1].type == X86_OP_IMM)
    for e in observations:
        if 'target' not in e or 'rip_after' not in e:
            continue
        pc = end_to_pc.get(image.rva(e['rip_after']))
        by_target[(e['target'], e.get('bytes', 0))].append({
            'qpc': e.get('qpc'), 'tid': e.get('tid'), 'writer_rva': pc,
            'immediate_constant_store': immediate_store(pc),
            'value': e.get('value') if e.get('read_ok') else None,
            'source': e['source'], 'read_ok': e.get('read_ok', False)})
    result = []
    for (target, width), events in sorted(by_target.items()):
        events.sort(key=lambda e: (e['qpc'] is None, e['qpc'] or 0))
        threads = {e['tid'] for e in events}
        complete = all(e['qpc'] is not None and e['writer_rva'] is not None and e['read_ok'] for e in events)
        ordered = complete and len(threads) == 1 and len({e['qpc'] for e in events}) == len(events)
        per_thread = defaultdict(list)
        for e in events:
            per_thread[e['tid']].append(e)
        sequences = []
        for tid, rows in per_thread.items():
            local_order = all(e['qpc'] is not None and e['writer_rva'] is not None for e in rows)
            local_order &= len({e['qpc'] for e in rows}) == len(rows)
            for i, e in enumerate(rows):
                e['observed_role'] = ('initialization_candidate' if e['immediate_constant_store']
                                      else 'update_candidate' if i and local_order
                                      else 'observed_write') if local_order else 'order_ambiguous'
            sequences.append({'tid': tid, 'writer_rvas': [e['writer_rva'] for e in rows],
                              'order_status': 'observed_thread_order' if local_order else 'ambiguous'})
        result.append({'target': target, 'bytes': width, 'order_status':
                       'observed_single_thread_order' if ordered else 'ambiguous',
                       'thread_sequences': sequences, 'events': events, 'limitation':
                       'watch events sample one address; first sampled event may follow unseen writes; '
                       'final value, whole-range and cross-thread ordering are unproven'})
    return result


def analyze_capture(root, image=None, budgets=Budgets(), selection=None):
    code, seeds, observations, identity = load_capture(root, image, budgets, selection)
    graph = analyze_graph(code, seeds, budgets)
    graph.update({'status': 'bounded_discovery_only', 'module_base': code.base,
                  'image_identity': identity,
                  'seed_rvas': [code.rva(s) for s in seeds],
                  'writers': ordered_writers(observations, code, graph['nodes']),
                  'replacement_allowed': False,
                  'global_limitations': ['static memory addresses require runtime registers',
                                         'control-flow edges are possible paths, not observed execution',
                                         'calls may be non-returning; fallthrough is assumed',
                                         'memory ownership and publication are unproven']})
    return graph


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', required=True, type=Path)
    parser.add_argument('--image', type=Path, help='optional captured PE module image')
    parser.add_argument('--selection', type=Path, help='automatically selected candidate entries')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--max-functions', type=int, default=24)
    parser.add_argument('--max-depth', type=int, default=3)
    parser.add_argument('--max-instructions', type=int, default=4096)
    args = parser.parse_args()
    budgets = Budgets(max_functions=args.max_functions, max_depth=args.max_depth,
                      max_instructions=args.max_instructions)
    if min(budgets.max_functions, budgets.max_depth + 1, budgets.max_instructions) < 1:
        parser.error('budgets must be positive')
    result = analyze_capture(args.evidence, args.image, budgets, args.selection)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'functions': len(result['functions']), 'nodes': len(result['nodes']),
                      'limitations': len(result['limitations']), 'out': str(args.out)}))


if __name__ == '__main__':
    main()
