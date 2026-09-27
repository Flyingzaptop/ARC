"""Typed DAG export for value-checked composite traces."""
import struct
from cpu_composite_ir import GPRS, Unclosed

def typed_export(machine):
    """Normalize byte tags to typed DAG references without guessing layout."""
    typed=[];old_to_new={};entry_cache={};slice_cache={}
    def kind(op,n):
        if op in ('vaddss','vsubss','vsubps','vsqrtps','vdpps','vinsertps',
                  'vcvtph2ps','vmaxss','vdivss','vmulss'):
            return 'f32' if n==4 else 'f32x4' if n==16 else 'bytes'+str(n)
        return {1:'u8',2:'u16',4:'u32',8:'u64',16:'u32x4'}.get(n,'bytes'+str(n))
    def add(op,raw,inputs=(),extra=None):
        node={'id':len(typed),'op':op,'type':kind(op,len(raw)),'bytes':raw.hex(),'inputs':list(inputs)}
        if extra:node.update(extra)
        typed.append(node);return node['id']
    def entry(tag):
        family=tag[0];key=tag[1];cache=(family,key)
        if cache not in entry_cache:
            if family=='entry_register':
                raw=struct.pack('<Q',machine.initial['registers'][GPRS.index(key)])
            elif family=='entry_vector':
                raw=machine.initial['xmm_bytes'][key*16:key*16+16]
            else:raise Unclosed('typed entry tag')
            entry_cache[cache]=add(family,raw,extra={'name':key,'slot':None})
        return entry_cache[cache]
    def node_ref(old):
        if old in old_to_new:return old_to_new[old]
        source=machine.nodes[old]
        ins=[sequence(part) for part in source['inputs']]
        extra={k:v for k,v in source.items() if k not in ('id','op','inputs','bytes')}
        if source['op']=='memory_input':extra['slot']=None
        new=add(source['op'],bytes.fromhex(source['bytes']),ins,extra)
        old_to_new[old]=new;return new
    def byte_ref(tag):
        if isinstance(tag[0],int):return node_ref(tag[0]),tag[1]
        if tag[0] in ('entry_register','entry_vector'):return entry(tag),tag[2]
        if tag[0]=='zero':return add('constant',b'\0'),0
        raise Unclosed('unresolved byte tag')
    def sequence(tags):
        if not tags:raise Unclosed('empty typed operand')
        refs=[byte_ref(t) for t in tags]
        parent,offset=refs[0]
        if all(ref==(parent,offset+j) for j,ref in enumerate(refs)):
            if offset==0 and len(tags)==len(bytes.fromhex(typed[parent]['bytes'])):return parent
            key=(parent,offset,len(tags))
            if key not in slice_cache:
                raw=bytes.fromhex(typed[parent]['bytes'])[offset:offset+len(tags)]
                slice_cache[key]=add('extract_bytes',raw,[parent],{'byte_offset':offset,'width':len(tags)})
            return slice_cache[key]
        raw=bytes(bytes.fromhex(typed[p]['bytes'])[j] for p,j in refs)
        return add('pack_bytes',raw,[{'id':p,'byte_offset':j} for p,j in refs])
    ordered=[];external_addresses=set()
    for write in machine.writes:
        if write['region']!='external':continue
        ordered.append({'address':write['address'],'width':write['size'],'bytes':write['bytes'],
                        'source':sequence(write['tags']),'slot':None})
        external_addresses.update(range(write['address'],write['address']+write['size']))
    outputs=[];addresses=sorted(external_addresses)
    groups=[]
    for address in addresses:
        if not groups or address!=groups[-1][-1]+1:groups.append([address])
        else:groups[-1].append(address)
    for group in groups:
        raw=bytes(machine.shadow[a][0] for a in group)
        tags=[machine.shadow[a][1] for a in group]
        outputs.append({'address':group[0],'width':len(group),'bytes':raw.hex(),
                        'source':sequence(tags),'slot':None})
    atoms=[]
    def owner(tag):
        if isinstance(tag[0],int):return ('node',tag[0]),tag[1]
        if tag[0] in ('entry_register','entry_vector'):return (tag[0],tag[1]),tag[2]
        return (tag[0],),None
    for group in groups:
        run=[]
        for address in group:
            tag=machine.shadow[address][1]
            if run:
                previous=machine.shadow[run[-1]][1];a,ai=owner(previous);b,bi=owner(tag)
                if a!=b or ai is None or bi!=ai+1:
                    tags=[machine.shadow[x][1] for x in run]
                    atoms.append({'address':run[0],'width':len(run),
                                  'bytes':bytes(machine.shadow[x][0] for x in run).hex(),
                                  'source':sequence(tags),'slot':None});run=[]
            run.append(address)
        if run:
            tags=[machine.shadow[x][1] for x in run]
            atoms.append({'address':run[0],'width':len(run),
                          'bytes':bytes(machine.shadow[x][0] for x in run).hex(),
                          'source':sequence(tags),'slot':None})
    guards=[]
    for guard in machine.guards:
        flag=guard.get('flag_inputs')
        guards.append({**guard,'source':sequence(flag) if flag else None})
    conditionals=[{'pc':item['pc'],'predicate':item['predicate'],
                   'observed_taken':item['observed_taken'],
                   'source':sequence(item['source']),
                   'flag_source':sequence(item['flag_inputs'])}
                  for item in machine.conditionals]
    accesses=[{'kind':a['kind'],'address':a['address'],'size':a['size'],
               'address_source':sequence(a['address_tags']),'value_source':sequence(a['value_tags'])}
              for a in machine.memory_accesses]
    return {'schema':1,'layout_status':'unresolved_absolute_capture_addresses',
            'nodes':typed,'outputs':outputs,'output_atoms':atoms,
            'ordered_external_writes':ordered,'guards':guards,'conditionals':conditionals,
            'memory_accesses':accesses,
            'lowering_allowed':False,
            'missing':['stable per-record input slots','external output address recipe','branch and side-effect closure across alternate paths']}
