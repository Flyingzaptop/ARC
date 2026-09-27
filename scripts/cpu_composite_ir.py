"""Bounded, value-checked provenance for one observed x64 path.

Every accepted node is replayed against the next saved context. Unsupported
instructions and missing memory values remain explicit; this is not a live
replacement contract or a whole-function proof.
"""
import argparse
import copy
import json
import math
import struct
from pathlib import Path

import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from cpu_contract_events import event_records
from cpu_producer_plan import GPRS


class Unclosed(ValueError):
    pass


ALIASES={}
for name in GPRS:
    if name.startswith('r') and name[1:].isdigit():
        k=name[1:];names=[name,'r'+k+'d','r'+k+'w','r'+k+'b']
    else:
        root=name[1:];low={'ax':'al','cx':'cl','dx':'dl','bx':'bl','sp':'spl','bp':'bpl','si':'sil','di':'dil'}[root]
        names=[name,'e'+root,root,low]
    for n,size in zip(names,(8,4,2,1)):ALIASES[n]=(name,0,size)
for low,base in [('ah','rax'),('ch','rcx'),('dh','rdx'),('bh','rbx')]:ALIASES[low]=(base,1,1)


def f32(value):
    try:return struct.unpack('<f',struct.pack('<f',value))[0]
    except OverflowError as exc:raise Unclosed('float32 overflow') from exc


class Machine:
    def __init__(self,initial,meta):
        if initial['mxcsr']&(0x6000|0x8040):
            raise Unclosed('unsupported MXCSR rounding/FTZ/DAZ mode')
        self.initial=initial
        self.reg={n:bytearray(struct.pack('<Q',v)) for n,v in zip(GPRS,initial['registers'])}
        self.xmm={f'xmm{i}':bytearray(initial['xmm_bytes'][i*16:i*16+16]) for i in range(16)}
        self.tags={n:[('entry_register',n,j) for j in range(8)] for n in GPRS}
        self.tags.update({f'xmm{i}':[('entry_vector',i,j) for j in range(16)] for i in range(16)})
        self.shadow={};self.nodes=[];self.writes=[];self.guards=[];self.conditionals=[];self.memory_accesses=[];self.meta=meta
        self.post_verified_writes=0;self.post_verified_external_bytes=0
        self.post_verified_byte_addresses=set()
        self.flags=initial['eflags'];self.flag_expr=None;self.changed=set()
        self.flag_mask=0

    def node(self,op,inputs,raw,extra=None):
        n=len(self.nodes)
        self.nodes.append({'id':n,'op':op,'inputs':copy.deepcopy(inputs),'bytes':raw.hex(),**(extra or {})})
        return [(n,j) for j in range(len(raw))]

    def reg_read(self,name):
        if name in self.xmm:return bytes(self.xmm[name]),list(self.tags[name])
        if name not in ALIASES:raise Unclosed('register '+name)
        key,offset,size=ALIASES[name]
        return bytes(self.reg[key][offset:offset+size]),list(self.tags[key][offset:offset+size])

    def reg_write(self,name,data,tags):
        if name in self.xmm:
            if len(data) not in (4,8,16):raise Unclosed('vector width')
            self.xmm[name][:len(data)]=data;self.tags[name][:len(data)]=tags
            if len(data)<16:self.xmm[name][len(data):]=bytes(16-len(data));self.tags[name][len(data):]=[('zero',)]*(16-len(data))
            self.changed.add(name);return
        if name not in ALIASES:raise Unclosed('register '+name)
        key,offset,size=ALIASES[name]
        if len(data)!=size:raise Unclosed('register write width')
        self.reg[key][offset:offset+size]=data;self.tags[key][offset:offset+size]=tags
        if size==4 and offset==0:self.reg[key][4:]=bytes(4);self.tags[key][4:]=[('zero',)]*4
        self.changed.add(key)

    def ea(self,x,op,event=None):
        m=op.mem
        def r(reg):
            if not reg:return 0
            name=x.reg_name(reg)
            return x.address+x.size if name=='rip' else int.from_bytes(self.reg_read(name)[0],'little')
        segment=x.reg_name(m.segment) if m.segment else ''
        if segment and segment!='gs':raise Unclosed('unsupported segment '+segment)
        segment_base=event['teb'] if segment=='gs' and event is not None else 0
        return (segment_base+r(m.base)+r(m.index)*m.scale+m.disp)&((1<<64)-1)

    def address_tags(self,x,op,event,address):
        m=op.mem;inputs=[]
        if m.base:
            base=x.reg_name(m.base)
            if base=='rip':
                raw=struct.pack('<Q',x.address+x.size)
                inputs.append(self.node('code_address',[],raw))
            else:inputs.append(self.reg_read(base)[1])
        if m.index:inputs.append(self.reg_read(x.reg_name(m.index))[1])
        segment=x.reg_name(m.segment) if m.segment else None
        if segment=='gs':
            if event is None:raise Unclosed('segment context unavailable')
            inputs.append(self.node('captured_teb',[],struct.pack('<Q',event['teb'])))
        recipe={'base':x.reg_name(m.base) if m.base else None,
                'index':x.reg_name(m.index) if m.index else None,
                'scale':m.scale,'disp':m.disp,'segment':segment}
        return self.node('address',inputs,struct.pack('<Q',address),{'recipe':recipe})

    def stack_access(self,kind,address,size,tags):
        address_tags=self.node('address',[self.tags['rsp']],struct.pack('<Q',address),
                               {'recipe':{'base':'rsp','index':None,'scale':1,'disp':0,'segment':None}})
        self.memory_accesses.append({'kind':kind,'address':address,'size':size,
                                     'address_tags':address_tags,'value_tags':tags})

    def read(self,x,op,event,size=None):
        size=size or op.size
        if op.type==X86_OP_REG:
            raw,tags=self.reg_read(x.reg_name(op.reg));return raw[:size],tags[:size]
        if op.type==X86_OP_IMM:
            raw=(op.imm&((1<<(size*8))-1)).to_bytes(size,'little')
            return raw,self.node('immediate',[],raw,{'value':op.imm})
        if op.type!=X86_OP_MEM:raise Unclosed('operand kind')
        address=self.ea(x,op,event)
        address_tags=self.address_tags(x,op,event,address)
        if not event.get('memory_known',False):raise Unclosed('memory value unavailable')
        captured=next((m for m in event.get('memory',[]) if m['address']==address and m['size']>=size and m['ok']),None)
        if captured is None:raise Unclosed('memory operand unclosed')
        actual=bytes.fromhex(captured['bytes'])[:size]
        if any(address+j in self.shadow and self.shadow[address+j][0]!=actual[j] for j in range(size)):
            raise Unclosed('shadow/captured memory mismatch')
        if all(address+j in self.shadow for j in range(size)):
            tags=[self.shadow[address+j][1] for j in range(size)]
            self.memory_accesses.append({'kind':'read','address':address,'size':size,'address_tags':address_tags,'value_tags':tags})
            return actual,tags
        m=op.mem
        recipe={'base':x.reg_name(m.base) if m.base else None,
                'index':x.reg_name(m.index) if m.index else None,
                'scale':m.scale,'disp':m.disp,
                'segment':x.reg_name(m.segment) if m.segment else None}
        leaves=self.node('memory_input',[],actual,{'address':address,'size':size,'address_recipe':recipe})
        tags=[self.shadow[address+j][1] if address+j in self.shadow else leaves[j] for j in range(size)]
        self.memory_accesses.append({'kind':'read','address':address,'size':size,'address_tags':address_tags,'value_tags':tags})
        return actual,tags

    def write(self,x,op,event,raw,tags):
        if op.type==X86_OP_REG:self.reg_write(x.reg_name(op.reg),raw,tags);return
        if op.type!=X86_OP_MEM:raise Unclosed('write operand')
        address=self.ea(x,op,event)
        address_tags=self.address_tags(x,op,event,address)
        if event.get('memory_known'):
            captured=next((m for m in event.get('memory',[]) if m['address']==address and m['size']>=len(raw) and m['ok']),None)
            if captured and bytes.fromhex(captured['bytes'])[:len(raw)]!=bytes(self.shadow.get(address+j,(bytes.fromhex(captured['bytes'])[j],None))[0] for j in range(len(raw))):
                raise Unclosed('write preimage mismatch')
        for j,b in enumerate(raw):self.shadow[address+j]=(b,tags[j])
        self.writes.append({'address':address,'size':len(raw),'bytes':raw.hex(),'tags':tags,
                            'region':'stack' if self.meta.get('stack_low',0)<=address<self.meta.get('stack_high',0) else 'external',
                            'explicit':True})
        if self.writes[-1]['region']=='external':
            self.post_verified_byte_addresses.difference_update(range(address,address+len(raw)))
        self.memory_accesses.append({'kind':'write','address':address,'size':len(raw),'address_tags':address_tags,'value_tags':tags})

    def check(self,next_event):
        for name in self.changed:
            if name.startswith('xmm'):
                i=int(name[3:]);expected=next_event['xmm_bytes'][i*16:i*16+16];actual=bytes(self.xmm[name])
            else:
                expected=struct.pack('<Q',next_event['registers'][GPRS.index(name)]);actual=bytes(self.reg[name])
            if actual!=expected:raise Unclosed('next-context mismatch '+name)
        self.changed.clear()
        if self.flag_mask and (self.flags^next_event['eflags'])&self.flag_mask:
            raise Unclosed('next-context flags mismatch')
        self.flag_mask=0

    def step(self,event,next_event):
        code=bytes.fromhex(event['code'])
        d=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);d.detail=True
        decoded=next(d.disasm(code,event['rip']),None)
        if decoded is None:raise Unclosed('instruction decode mismatch')
        x=decoded;ops=x.operands;m=x.mnemonic;write_start=len(self.writes)
        def get(i,size=None):return self.read(x,ops[i],event,size)
        def put(i,raw,tags):self.write(x,ops[i],event,raw,tags)
        if m in ('mov','vmovaps','vmovups','vmovsd','vmovss','vmovd'):
            if len(ops)==2:
                size=(8 if m=='vmovsd' else 4 if m in ('vmovss','vmovd') else ops[0].size)
                raw,tags=get(1,size)
                put(0,raw,tags)
            elif m=='vmovss' and len(ops)==3:
                # VEX scalar merge: low dword from operand 2, upper lanes from operand 1.
                low,lt=get(2,4);upper,ut=get(1,16)
                put(0,low+upper[4:],lt+ut[4:])
            else:raise Unclosed('move arity')
        elif m=='movzx':
            raw,tags=get(1);size=ops[0].size;put(0,raw.ljust(size,b'\0'),tags+[('zero',)]*(size-len(raw)))
        elif m in ('vpxor','vxorps'):
            if len(ops)!=3:raise Unclosed('vector xor arity')
            a,at=get(1,16);b,bt=get(2,16)
            raw=bytes(x^y for x,y in zip(a,b));put(0,raw,self.node(m,[at,bt],raw))
        elif m in ('vaddss','vsubss'):
            if len(ops)!=3:raise Unclosed('scalar float arity')
            a,at=get(1,16);b,bt=get(2,4)
            av=struct.unpack_from('<f',a)[0];bv=struct.unpack('<f',b)[0]
            low=struct.pack('<f',f32(av+bv if m=='vaddss' else av-bv))
            raw=low+a[4:];put(0,raw,self.node(m,[at,bt],raw))
        elif m=='vsubps':
            if len(ops)!=3:raise Unclosed('packed subtract arity')
            a,at=get(1,16);b,bt=get(2,16)
            raw=b''.join(struct.pack('<f',f32(struct.unpack_from('<f',a,j)[0]-struct.unpack_from('<f',b,j)[0])) for j in (0,4,8,12))
            put(0,raw,self.node(m,[at,bt],raw))
        elif m=='vsqrtps':
            if len(ops)!=2:raise Unclosed('sqrt arity')
            a,at=get(1,16)
            if any(struct.unpack_from('<f',a,j)[0]<0 for j in (0,4,8,12)):
                raise Unclosed('negative sqrt lane')
            raw=b''.join(struct.pack('<f',f32(math.sqrt(struct.unpack_from('<f',a,j)[0]))) for j in (0,4,8,12))
            put(0,raw,self.node(m,[at],raw))
        elif m=='vdpps':
            if len(ops)!=4 or ops[3].type!=X86_OP_IMM:raise Unclosed('dot product mode')
            a,at=get(1,16);b,bt=get(2,16);mask=ops[3].imm&255
            terms=[f32(struct.unpack_from('<f',a,j*4)[0]*struct.unpack_from('<f',b,j*4)[0]) if mask&(1<<(j+4)) else 0.0 for j in range(4)]
            dot=f32(f32(terms[0]+terms[1])+f32(terms[2]+terms[3]))
            raw=b''.join(struct.pack('<f',dot if mask&(1<<j) else 0.0) for j in range(4))
            put(0,raw,self.node(m,[at,bt],raw,{'mask':mask,'reduction':'pairwise_float32'}))
        elif m=='vinsertps':
            if len(ops)!=4 or ops[3].type!=X86_OP_IMM:raise Unclosed('insert mode')
            base,bt=get(1,16);source,st=get(2,ops[2].size)
            imm=ops[3].imm&255;src_lane=((imm>>6)&3) if len(source)==16 else 0;dst_lane=(imm>>4)&3
            raw=bytearray(base);raw[dst_lane*4:dst_lane*4+4]=source[src_lane*4:src_lane*4+4]
            for j in range(4):
                if imm&(1<<j):raw[j*4:j*4+4]=bytes(4)
            put(0,bytes(raw),self.node(m,[bt,st],bytes(raw),{'control':imm}))
        elif m=='vcomiss':
            if len(ops)!=2:raise Unclosed('float compare arity')
            a,at=get(0,4);b,bt=get(1,4);av=struct.unpack('<f',a)[0];bv=struct.unpack('<f',b)[0]
            z=math.isnan(av) or math.isnan(bv) or av==bv
            cf=math.isnan(av) or math.isnan(bv) or av<bv
            pf=math.isnan(av) or math.isnan(bv)
            self.flags=(self.flags&~(0x45))|(0x40 if z else 0)|(1 if cf else 0)|(4 if pf else 0)
            self.flag_expr=self.node(m,[at,bt],bytes([int(z),int(cf),int(pf)]))
            self.flag_mask=0x45
        elif m=='vcvtps2ph':
            if len(ops)!=3 or ops[2].type!=X86_OP_IMM or ops[2].imm!=0:
                raise Unclosed('half conversion mode')
            source,tags=get(1,16)
            try:low=b''.join(struct.pack('<e',struct.unpack_from('<f',source,i)[0]) for i in (0,4,8,12))
            except (OverflowError,struct.error) as exc:raise Unclosed('half conversion unsupported value') from exc
            raw=low.ljust(ops[0].size,b'\0');put(0,raw,self.node(m,[tags],raw,{'rounding':'nearest_even'}))
        elif m=='vpextrw':
            if len(ops)!=3 or ops[2].type!=X86_OP_IMM:raise Unclosed('extract word mode')
            source,tags=get(1,16);index=ops[2].imm&7
            put(0,source[index*2:index*2+2],tags[index*2:index*2+2])
        elif m=='vcvtph2ps':
            if len(ops)!=2 or ops[0].size!=16:raise Unclosed('half to float mode')
            source,tags=get(1,8)
            raw=b''.join(struct.pack('<f',struct.unpack_from('<e',source,j)[0]) for j in (0,2,4,6))
            put(0,raw,self.node(m,[tags],raw))
        elif m in ('vmaxss','vdivss','vmulss'):
            if len(ops)!=3:raise Unclosed('scalar arithmetic arity')
            a,at=get(1,16);b,bt=get(2,4)
            av=struct.unpack_from('<f',a)[0];bv=struct.unpack('<f',b)[0]
            if m=='vmaxss':value=av if av>bv else bv
            elif m=='vdivss':
                if bv==0:raise Unclosed('division by zero')
                value=f32(av/bv)
            else:value=f32(av*bv)
            raw=struct.pack('<f',value)+a[4:]
            put(0,raw,self.node(m,[at,bt],raw))
        elif m=='vcvttss2si':
            if len(ops)!=2:raise Unclosed('float integer conversion arity')
            source,tags=get(1,4);value=struct.unpack('<f',source)[0];bits=ops[0].size*8
            if not math.isfinite(value) or not -(1<<(bits-1))<=value<(1<<(bits-1)):
                raise Unclosed('float integer conversion range')
            result=math.trunc(value);raw=(result&((1<<bits)-1)).to_bytes(ops[0].size,'little')
            put(0,raw,self.node(m,[tags],raw,{'rounding':'toward_zero'}))
        elif m=='lea':
            address=self.ea(x,ops[1]);size=ops[0].size;raw=(address&((1<<(size*8))-1)).to_bytes(size,'little')
            mem=ops[1].mem;parts=[]
            if mem.base:
                base=x.reg_name(mem.base)
                parts.append(self.node('code_address',[],struct.pack('<Q',x.address+x.size)) if base=='rip' else self.reg_read(base)[1])
            if mem.index:parts.append(self.reg_read(x.reg_name(mem.index))[1])
            recipe={'base':x.reg_name(mem.base) if mem.base else None,
                    'index':x.reg_name(mem.index) if mem.index else None,
                    'scale':mem.scale,'disp':mem.disp}
            put(0,raw,self.node('lea',parts,raw,{'recipe':recipe}))
        elif m in ('add','sub','and','or','shl','shr','xor'):
            left,lt=get(0);right,rt=get(1,len(left));bits=len(left)*8;mask=(1<<bits)-1;a=int.from_bytes(left,'little');b=int.from_bytes(right,'little')
            value={'add':lambda:(a+b)&mask,'sub':lambda:(a-b)&mask,'and':lambda:a&b,'or':lambda:a|b,
                   'shl':lambda:(a<<(b&63))&mask,'shr':lambda:a>>(b&63),'xor':lambda:a^b}[m]()
            raw=value.to_bytes(len(left),'little');tags=self.node(m,[lt,rt],raw);put(0,raw,tags)
            self.flags=(self.flags&~0x40)|(0x40 if value==0 else 0)
            self.flag_expr=tags;self.flag_mask=0x40
        elif m=='inc':
            before,tags=get(0);size=len(before);value=(int.from_bytes(before,'little')+1)&((1<<(size*8))-1)
            raw=value.to_bytes(size,'little');out=self.node(m,[tags],raw);put(0,raw,out)
            self.flags=(self.flags&~0x40)|(0x40 if value==0 else 0);self.flag_expr=out;self.flag_mask=0x40
        elif m=='bsf':
            source,tags=get(1);value=int.from_bytes(source,'little')
            if not value:raise Unclosed('bsf zero destination undefined')
            index=(value&-value).bit_length()-1;raw=index.to_bytes(ops[0].size,'little')
            put(0,raw,self.node(m,[tags],raw));self.flags&=~0x40
            self.flag_expr=self.node('bsf_flags',[tags],b'\0');self.flag_mask=0x40
        elif m=='btc':
            before,bt=get(0);bit_raw,it=get(1,len(before));bits=len(before)*8
            index=int.from_bytes(bit_raw,'little')&(bits-1);value=int.from_bytes(before,'little')
            carry=(value>>index)&1;result=value^(1<<index);raw=result.to_bytes(len(before),'little')
            put(0,raw,self.node(m,[bt,it],raw));self.flags=(self.flags&~1)|carry
            self.flag_expr=self.node('btc_flags',[bt,it],bytes([carry]));self.flag_mask=1
        elif m in ('cmova','cmovne'):
            zf=bool(event['eflags']&0x40);cf=bool(event['eflags']&1)
            take=(not zf and not cf) if m=='cmova' else not zf
            if not self.flag_expr:raise Unclosed('cmov condition provenance unavailable')
            old,old_tags=get(0);new,new_tags=get(1,len(old))
            raw=new if take else old
            tags=self.node('select',[self.flag_expr,new_tags,old_tags],raw,
                           {'condition':'above' if m=='cmova' else 'nonzero',
                            'observed_taken':take,'source_instruction':m})
            put(0,raw,tags)
            self.conditionals.append({'pc':x.address,'predicate':m,'observed_taken':take,
                                      'source':tags,'flag_inputs':self.flag_expr})
        elif m=='nop':
            pass
        elif m in ('cmp','test'):
            a,at=get(0);b,bt=get(1,len(a));av=int.from_bytes(a,'little');bv=int.from_bytes(b,'little');bits=len(a)*8;mask=(1<<bits)-1
            result=(av-bv)&mask if m=='cmp' else av&bv
            z=result==0;cf=av<bv if m=='cmp' else False;sf=bool(result&(1<<(bits-1)));of=bool(((av^bv)&(av^result))&(1<<(bits-1))) if m=='cmp' else False
            self.flags=(self.flags&~(0x8c1))|(0x40 if z else 0)|(1 if cf else 0)|(0x80 if sf else 0)|(0x800 if of else 0)
            self.flag_expr=self.node(m,[at,bt],result.to_bytes(len(a),'little'))
            self.flag_mask=0x8c1
        elif m in ('je','jne','ja','jbe','jmp'):
            target=ops[0].imm;zf=bool(event['eflags']&0x40);cf=bool(event['eflags']&1)
            taken={'je':zf,'jne':not zf,'ja':not (cf or zf),'jbe':cf or zf,'jmp':True}[m]
            expected=target if taken else x.address+x.size
            if next_event['rip']!=expected:raise Unclosed('branch path mismatch')
            self.guards.append({'pc':x.address,'predicate':m,'taken':taken,'flag_inputs':self.flag_expr})
        elif m=='push':
            raw,tags=get(0,8);old=int.from_bytes(self.reg['rsp'],'little');new=(old-8)&((1<<64)-1)
            self.reg_write('rsp',struct.pack('<Q',new),self.node('stack_adjust',[self.tags['rsp']],struct.pack('<Q',new)))
            # Implicit memory operand is not in the capture plan as an x86 operand.
            for j,b in enumerate(raw):self.shadow[new+j]=(b,tags[j])
            self.writes.append({'address':new,'size':8,'bytes':raw.hex(),'tags':tags,'region':'stack','explicit':False})
            self.stack_access('write',new,8,tags)
        elif m=='pop':
            if len(ops)!=1 or ops[0].size!=8 or x.addr_size!=8:raise Unclosed('unsupported pop width/address mode')
            old=int.from_bytes(self.reg['rsp'],'little')
            if not all(old+j in self.shadow for j in range(8)):raise Unclosed('pop stack value unavailable')
            raw=bytes(self.shadow[old+j][0] for j in range(8));tags=[self.shadow[old+j][1] for j in range(8)]
            self.stack_access('read',old,8,tags)
            # POP reads the old stack, increments RSP, then resolves/writes its destination.
            new=(old+8)&((1<<64)-1);self.reg_write('rsp',struct.pack('<Q',new),self.node('stack_adjust',[self.tags['rsp']],struct.pack('<Q',new)))
            put(0,raw,tags)
        elif m=='ret':
            if not event.get('memory_known'):
                raise Unclosed('return boundary unclosed')
            old=int.from_bytes(self.reg['rsp'],'little')
            captured=next((item for item in event.get('memory',[]) if item['address']==old and item['size']>=8 and item['ok']),None)
            if captured is None or int.from_bytes(bytes.fromhex(captured['bytes'])[:8],'little')!=next_event['rip']:
                raise Unclosed('return target mismatch')
            self.stack_access('read',old,8,self.node('memory_input',[],bytes.fromhex(captured['bytes'])[:8],{'address':old,'size':8,'slot':None}))
            new=old+8;self.reg_write('rsp',struct.pack('<Q',new),self.node('stack_adjust',[self.tags['rsp']],struct.pack('<Q',new)))
            self.guards.append({'pc':x.address,'predicate':'ret','taken':True,'target':next_event['rip']})
        elif m=='call':
            if len(ops)!=1 or ops[0].type!=X86_OP_IMM or next_event['rip']!=ops[0].imm:
                raise Unclosed('indirect or mismatched call')
            return_address=x.address+x.size;old=int.from_bytes(self.reg['rsp'],'little');new=old-8
            raw=struct.pack('<Q',return_address);tags=self.node('return_address',[],raw)
            for j,b in enumerate(raw):self.shadow[new+j]=(b,tags[j])
            self.writes.append({'address':new,'size':8,'bytes':raw.hex(),'tags':tags,'region':'stack','explicit':False})
            self.reg_write('rsp',struct.pack('<Q',new),self.node('stack_adjust',[self.tags['rsp']],struct.pack('<Q',new)))
            self.stack_access('write',new,8,tags)
            self.guards.append({'pc':x.address,'predicate':'call','taken':True,'target':ops[0].imm})
        else:raise Unclosed('unsupported operation '+m)
        if 'previous_after' in next_event:
            for write in self.writes[write_start:]:
                if not write['explicit']:continue
                observed=next((item for item in next_event['previous_after'] if item['address']==write['address'] and item['size']>=write['size'] and item['ok']),None)
                if observed is None:raise Unclosed('write postimage unavailable')
                if bytes.fromhex(observed['bytes'])[:write['size']]!=bytes.fromhex(write['bytes']):
                    raise Unclosed('write postimage mismatch')
                self.post_verified_writes+=1
                if write['region']=='external':
                    self.post_verified_external_bytes+=write['size']
                    self.post_verified_byte_addresses.update(range(write['address'],write['address']+write['size']))
        self.check(next_event)
        return x

    def typed_export(self):
        from cpu_composite_export import typed_export
        return typed_export(self)


def _analyze_path(events,meta,by_rva):
    if len(events)<2:raise ValueError('insufficient events')
    machine=Machine(events[0],meta)
    completed=0;reason=None
    for event,next_event in zip(events,events[1:]):
        if event['kind']==2:break
        rva=event['rip']-meta['main_base'];record=by_rva.get(rva)
        if record is None or not event['code'].startswith(record['code']) or record['status'] not in ('covered','unsupported'):
            reason={'event':event['index'],'rva':rva,'reason':'code or memory plan not covered'};break
        try:machine.step(event,next_event)
        except (Unclosed,ValueError,OverflowError) as error:
            reason={'event':event['index'],'rva':rva,'reason':str(error)};break
        completed+=1
    return {'schema':1,'status':'closed_observed_path' if reason is None and completed==len(events)-1 else 'partial',
            'events':len(events),'validated_instructions':completed,'stop':reason,
            'post_verified_writes':machine.post_verified_writes,
            'post_verified_external_bytes':machine.post_verified_external_bytes,
            'post_verified_final_external_bytes':len(machine.post_verified_byte_addresses),
            'postimage_capture_available':any('previous_after' in e for e in events),
            'nodes':machine.nodes,'writes':machine.writes,'guards':machine.guards,
            'conditionals':machine.conditionals,
            'memory_accesses':machine.memory_accesses,
            'typed_ir':machine.typed_export(),
            'live_replacement_allowed':False,'gpu_lowering_complete':False,
            'limits':['only observed path','next-context value checks on modified registers',
                      'memory writes lacking previous_after are not postimage verified',
                      'upper YMM state and MXCSR exception effects not modeled',
                      'FP16/division/sqrt/NaN and signed-zero modes beyond captured values not certified',
                      'arbitrary other paths and cross-thread accesses unknown']}


def analyze_paths(folder):
    meta=json.loads((folder/'capture.json').read_text())
    plan=json.loads((folder/'memory-plan.json').read_text())
    by_rva={r['rva']:r for r in plan['records']}
    events=list(event_records(folder,meta,vectors=True))
    groups=[];current=[]
    for event in events:
        if event['kind']==1:
            if current:raise ValueError('nested or missing end event')
            current=[event]
        elif current:
            current.append(event)
            if event['kind']==2:groups.append(current);current=[]
        else:raise ValueError('event outside bounded path')
    if current:raise ValueError('unterminated path')
    if not groups:raise ValueError('no bounded paths')
    paths=[_analyze_path(group,meta,by_rva) for group in groups]
    return paths,groups


def analyze(folder):
    paths,groups=analyze_paths(folder)
    events_count=sum(len(group) for group in groups)
    if len(paths)==1:return paths[0]
    summaries=[{'first_event':group[0]['index'],'last_event':group[-1]['index'],
                'status':path['status'],'validated_instructions':path['validated_instructions'],
                'stop':path['stop'],'external_writes':sum(w['region']=='external' for w in path['writes']),
                'post_verified_writes':path['post_verified_writes'],
                'post_verified_external_bytes':path['post_verified_external_bytes'],
                'post_verified_final_external_bytes':path['post_verified_final_external_bytes'],
                'branch_pattern':[(g['predicate'],g['taken']) for g in path['guards']]}
               for group,path in zip(groups,paths)]
    return {'schema':1,'status':'closed_observed_paths' if all(p['status']=='closed_observed_path' for p in paths) else 'partial',
            'events':events_count,'path_count':len(paths),'paths':summaries,'representative':paths[0],
            'typed_ir':paths[0]['typed_ir'],'live_replacement_allowed':False,'gpu_lowering_complete':False,
            'limits':['representative typed IR is one observed path','other paths validated independently','live input and output binding unknown']}


def main():
    p=argparse.ArgumentParser();p.add_argument('candidate',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    result=analyze(a.candidate);a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:result.get(k) for k in ('status','events','path_count','validated_instructions','stop')}))


if __name__=='__main__':main()
