"""Conservative forward must-constant analysis on a decoded x64 function.

Unknown edges are reported, never treated as a whole-function proof. Calls kill
volatile registers; preservation of nonvolatile registers is an explicit ABI
assumption. Memory values are never promoted to constants from a recorded run.
"""
from collections import deque
import capstone
from capstone.x86 import X86_OP_REG, X86_OP_IMM

NONVOLATILE = {'rbx','rbp','rsi','rdi','r12','r13','r14','r15','rsp'}

def canonical(name):
    if name in NONVOLATILE | {'rax','rcx','rdx','r8','r9','r10','r11'}:
        return name
    aliases = {'eax':'rax','ax':'rax','al':'rax','ah':'rax',
               'ebx':'rbx','bx':'rbx','bl':'rbx','bh':'rbx',
               'ecx':'rcx','cx':'rcx','cl':'rcx','ch':'rcx',
               'edx':'rdx','dx':'rdx','dl':'rdx','dh':'rdx',
               'esi':'rsi','si':'rsi','sil':'rsi','edi':'rdi','di':'rdi','dil':'rdi',
               'esp':'rsp','sp':'rsp','spl':'rsp','ebp':'rbp','bp':'rbp','bpl':'rbp'}
    if name in aliases:return aliases[name]
    if name.startswith('r') and name[-1:] in ('d','w','b') and name[1:-1].isdigit():
        return name[:-1]
    return None

def transfer(i, state):
    out = dict(state)
    for reg in i.regs_access()[1]:
        out.pop(canonical(i.reg_name(reg)), None)
    if i.group(capstone.CS_GRP_CALL):
        return {r:v for r,v in out.items() if r in NONVOLATILE}
    ops = i.operands
    if not ops or ops[0].type != X86_OP_REG or ops[0].size not in (4,8):return out
    dst = canonical(i.reg_name(ops[0].reg))
    if dst is None:return out
    def read(op):
        if op.type == X86_OP_IMM:return op.imm
        if op.type == X86_OP_REG:
            name = i.reg_name(op.reg)
            if name in ('ah','bh','ch','dh'):return None
            v = state.get(canonical(name))
            return None if v is None else v & ((1 << (8*op.size))-1)
        return None
    value = None
    if len(ops)==2:
        a,b = read(ops[0]),read(ops[1])
        if i.mnemonic in ('mov','movabs','movzx','movsxd','movsx'):
            value = b
            if value is not None and i.mnemonic in ('movsx','movsxd'):
                width=ops[1].size*8
                if value & (1<<(width-1)):value -= 1<<width
        elif i.mnemonic=='xor' and ops[1].type==X86_OP_REG and ops[0].reg==ops[1].reg:
            value=0
        elif a is not None and b is not None:
            if i.mnemonic=='and':value=a&b
            elif i.mnemonic=='or':value=a|b
            elif i.mnemonic=='xor':value=a^b
            elif i.mnemonic in ('shl','sal'):value=a << (b & (63 if ops[0].size==8 else 31))
            elif i.mnemonic=='shr':value=a >> (b & (63 if ops[0].size==8 else 31))
    if value is not None:out[dst]=value & ((1<<(ops[0].size*8))-1)
    return out

def analyze(instructions):
    ins={i.address:i for i in instructions}
    if not instructions:return {}, []
    incoming={instructions[0].address:{}}
    pending=deque([instructions[0].address]);missing=set();steps=0
    while pending:
        pc=pending.popleft();steps+=1
        if steps>max(10000,len(ins)*100):raise ValueError('Constant propagation budget')
        i=ins[pc];out=transfer(i,incoming[pc]);edges=[]
        if i.group(capstone.CS_GRP_JUMP):
            if i.operands and i.operands[0].type==X86_OP_IMM:edges.append(i.operands[0].imm)
            else:missing.add(pc)
            if i.mnemonic!='jmp':edges.append(pc+i.size)
        elif not i.group(capstone.CS_GRP_RET):edges.append(pc+i.size)
        for target in edges:
            if target not in ins:missing.add(pc);continue
            old=incoming.get(target)
            merged=out.copy() if old is None else {r:v for r,v in old.items() if out.get(r)==v}
            if old is None or merged!=old:
                incoming[target]=merged;pending.append(target)
    return incoming,sorted(missing)
