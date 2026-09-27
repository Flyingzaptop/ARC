"""Build a bounded memory-operand lookup from selected, checked machine code.

memory-rvas.bin uses little endian: uint32 record_count; for each decoded
instruction uint32 rva, uint32 instruction_length, uint32 operand_count;
operand_count 0xffffffff means unsupported coverage. Each of 1-2 supported
operands is int32 base,index,scale; int64 displacement; uint32 size. Register
indices follow cpu_producer_plan.GPRS: -1 absent, 16 RIP. A missing PC is never
proof of no memory access; the native reader must treat it as unsupported.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import capstone
from capstone.x86_const import X86_OP_MEM

from cpu_producer_plan import memory as producer_memory

HEADER = struct.Struct('<I')
FUNCTION = struct.Struct('<II')
RECORD = struct.Struct('<III')
OPERAND = struct.Struct('<iiiqI')
UNSUPPORTED = 0xffffffff
MAX_RECORDS = 32768
MAX_FUNCTIONS = 128
MAX_INPUT_BYTES = 32 * 1024 * 1024


def checked_functions(path):
    data = Path(path).read_bytes()
    if len(data) < HEADER.size or len(data) > MAX_INPUT_BYTES:
        raise ValueError('checked function file outside input budget')
    count = HEADER.unpack_from(data)[0]
    if count > MAX_FUNCTIONS:
        raise ValueError('checked function count exceeds budget')
    offset = HEADER.size
    functions = []
    for _ in range(count):
        if offset + FUNCTION.size > len(data):
            raise ValueError('truncated checked function header')
        rva, size = FUNCTION.unpack_from(data, offset)
        offset += FUNCTION.size
        if not size or size > 65536 or offset + size > len(data):
            raise ValueError('invalid checked function size')
        functions.append((rva, data[offset:offset + size]))
        offset += size
    if offset != len(data):
        raise ValueError('trailing checked function bytes')
    return functions


def _access(ins, operand, encoded):
    access = operand.access
    read = bool(access & capstone.CS_AC_READ)
    if ins.mnemonic in ('test','cmp'):read=True
    write = bool(access & capstone.CS_AC_WRITE) or encoded['write']
    return {'read': read, 'write': write, 'access_known': read or write}


def _implicit_memory_reason(ins):
    # Capstone's explicit operands omit stack/string memory effects.
    if ins.mnemonic in ('pushf', 'pushfq', 'popf', 'popfq', 'enter', 'leave'):
        return 'implicit_memory_effect'
    strings = (
            'movsb', 'movsw', 'movsd', 'movsq', 'stosb', 'stosw', 'stosd', 'stosq',
            'lodsb', 'lodsw', 'lodsd', 'lodsq', 'scasb', 'scasw', 'scasd', 'scasq',
            'cmpsb', 'cmpsw', 'cmpsd', 'cmpsq')
    if ins.mnemonic in strings and not any(
            ins.reg_name(op.reg).startswith(('xmm', 'ymm', 'zmm'))
            for op in ins.operands if op.type == capstone.x86_const.X86_OP_REG):
        return 'implicit_memory_effect'
    if ins.group(capstone.CS_GRP_IRET):
        return 'implicit_memory_effect'
    return None


def _describe(ins):
    if reason := _implicit_memory_reason(ins):
        return {'status': 'unsupported', 'reason': reason, 'operands': []}
    stack = None
    if ins.group(capstone.CS_GRP_CALL) or ins.group(capstone.CS_GRP_RET) or ins.mnemonic in ('push', 'pop'):
        if 0x66 in ins.prefix or ins.mnemonic not in ('call', 'ret', 'push', 'pop'):
            return {'status': 'unsupported', 'reason': 'nonstandard_stack_width_or_form', 'operands': []}
        if ins.mnemonic == 'pop' and 0x67 in ins.prefix:
            return {'status': 'unsupported', 'reason': 'pop_address_size_override', 'operands': []}
        ops = ins.operands
        if ins.mnemonic in ('push', 'pop', 'call'):
            if len(ops) != 1 or (ops[0].type != capstone.x86_const.X86_OP_IMM and ops[0].size != 8):
                return {'status': 'unsupported', 'reason': 'nonstandard_stack_width_or_form', 'operands': []}
        elif len(ops) > 1 or (ops and ops[0].type != capstone.x86_const.X86_OP_IMM):
            return {'status': 'unsupported', 'reason': 'nonstandard_stack_width_or_form', 'operands': []}
        stack_write = ins.mnemonic in ('push', 'call')
        stack = {'base': 4, 'index': -1, 'scale': 1, 'disp': -8 if stack_write else 0,
                 'size': 8, 'read': not stack_write, 'write': stack_write,
                 'access_known': True, 'implicit': True}
    operands = [op for op in ins.operands if op.type == X86_OP_MEM] if ins.mnemonic not in ('lea','nop') else []
    if len(operands) + bool(stack) > 2:
        return {'status': 'unsupported', 'reason': 'more_than_two_memory_operands', 'operands': []}
    encoded = []
    for operand in operands:
        try:
            if operand.mem.segment:
                if ins.reg_name(operand.mem.segment) != 'gs' or operand.mem.base or operand.mem.index:
                    raise ValueError('unsupported segment-relative memory')
                item = {'base': 17, 'index': -1, 'scale': 1, 'disp': operand.mem.disp,
                        'size': operand.size, 'write': False}
            else:
                item = producer_memory(ins, operand)
        except (ValueError, IndexError) as error:
            return {'status': 'unsupported', 'reason': str(error), 'operands': []}
        if not 1 <= item['size'] <= 32:
            return {'status': 'unsupported', 'reason': 'memory_operand_size_outside_budget', 'operands': []}
        access = _access(ins, operand, item)
        if not access['access_known']:
            return {'status': 'unsupported', 'reason': 'memory_operand_access_unknown', 'operands': []}
        if ins.mnemonic == 'pop' and (item['base'] == 4 or item['index'] == 4):
            # POP increments RSP before evaluating a memory destination.
            # Native capture evaluates these descriptors from pre-instruction GPRs.
            item['disp'] += (8 if item['base'] == 4 else 0) + (8 * item['scale'] if item['index'] == 4 else 0)
        encoded.append({k: item[k] for k in ('base', 'index', 'scale', 'disp', 'size')} | access)
    if stack:
        encoded.append(stack)
    return {'status': 'covered', 'reason': None, 'operands': encoded}


def build(directory):
    directory = Path(directory)
    source = directory / 'checked-functions.bin'
    functions = checked_functions(source)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    records = {}
    gaps = []
    for begin, code in functions:
        cursor = begin
        for ins in decoder.disasm(code, begin):
            if ins.address != cursor:
                gaps.append({'begin_rva': cursor, 'end_rva': ins.address, 'reason': 'decode_gap'})
            cursor = ins.address + ins.size
            row = _describe(ins)
            row.update({'rva': ins.address, 'length': ins.size, 'code': ins.bytes.hex(),
                        'mnemonic': ins.mnemonic})
            old = records.get(ins.address)
            if old is not None and old != row:
                raise ValueError('overlapping checked functions disagree at RVA')
            records[ins.address] = row
            if len(records) > MAX_RECORDS:
                raise ValueError('memory map record budget exceeded')
        if cursor != begin + len(code):
            gaps.append({'begin_rva': cursor, 'end_rva': begin + len(code),
                         'reason': 'undecodable_checked_bytes'})
    ordered = [records[rva] for rva in sorted(records)]
    out = bytearray(HEADER.pack(len(ordered)))
    for row in ordered:
        operands = row['operands']
        count = len(operands) if row['status'] == 'covered' else UNSUPPORTED
        out.extend(RECORD.pack(row['rva'], row['length'], count))
        for operand in operands:
            out.extend(OPERAND.pack(*(operand[k] for k in ('base', 'index', 'scale', 'disp', 'size'))))
    (directory / 'memory-rvas.bin').write_bytes(out)
    result = {'schema': 1, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'binary_format': 'little_endian: u32 count; each u32 rva,u32 length,u32 n; '
                               'n=0xffffffff unsupported; n<=2 operands i32 base,index,scale,i64 disp,u32 size',
              'missing_pc': 'unsupported', 'record_count': len(ordered),
              'unsupported_count': sum(row['status'] != 'covered' for row in ordered),
              'decode_gaps': gaps, 'coverage_status':
              'complete_decoded_operand_map' if not gaps and all(row['status'] == 'covered' for row in ordered)
              else 'missing_or_unsupported', 'records': ordered}
    (directory / 'memory-plan.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, help='selected candidate directory')
    args = parser.parse_args()
    result = build(args.directory)
    print(json.dumps({k: result[k] for k in ('record_count', 'unsupported_count', 'coverage_status')}))


if __name__ == '__main__':
    main()
