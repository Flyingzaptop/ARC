"""Generated bounded native snapshot gather emitter."""
import hashlib
import json
from pathlib import Path
from cpu_composite_ir import analyze_paths
from cpu_producer_plan import GPRS
from cpu_snapshot_gather import Unsupported, packet_proofs, first_read_origins, load_manifest

def emit_native(candidate,contract_path,manifest_path,output_dir,*,optimized=False,
                separate_diagnostics=False,packet_kind='append',word_proof_path=None):
    """Generate straight-line C++ for owned before-view reads; never run it here."""
    candidate=Path(candidate);output_dir=Path(output_dir)
    if separate_diagnostics and not optimized:raise Unsupported('diagnostic split requires optimized gather')
    manifest=load_manifest(manifest_path);contract=json.loads(Path(contract_path).read_text())
    if packet_kind=='append':proof=packet_proofs(candidate,manifest_path)
    elif packet_kind=='word_scatter':
        if word_proof_path is None:raise Unsupported('word-scatter proof required')
        proof=json.loads(Path(word_proof_path).read_text())
        if proof.get('kind')!='word_scatter' or proof.get('status')!='counted_word_scatter_snapshot_verified' or Path(proof['candidate']).resolve()!=candidate.resolve():
            raise Unsupported('word-scatter proof identity/status')
    else:raise Unsupported('unknown packet kind')
    models,event_groups=analyze_paths(candidate)
    typed=models[0]['typed_ir'];nodes=typed['nodes'];origins=first_read_origins(typed)
    leaves=contract['inputs'];words=contract.get('input_words_per_call',contract.get('input_words_per_row'))
    if not isinstance(words,int) or not 0<words<=64:raise Unsupported('shader input word budget')
    if any(item['node']>=len(nodes) or item['origin']!=nodes[item['node']]['op'] for item in leaves):
        raise Unsupported('shader leaf contract/topology mismatch')
    # No per-row CPU cursor or terminal bound may enter the shader input stream.
    if packet_kind=='append':
        preflight_addresses={manifest['cursor_address'],manifest['cursor_address']+8,
                             proof['loop_bound_proof']['bound_read_address']}
        carried_register=manifest['induction_register']
    else:
        preflight_addresses={item['address'] for item in proof['counter_preflight']}
        preflight_addresses.add(proof['binary_flag_preflight']['address'])
        preflight_addresses.add(proof['output_preflight']['base_pointer_read_address'])
        carried_register=proof['output_preflight']['index_register']
    for item in leaves:
        node=nodes[item['node']]
        if node['op']=='memory_input' and node['address'] in preflight_addresses:
            raise Unsupported('per-row cursor/capacity/terminal leaf remains in shader contract')
        if node['op']=='entry_register' and node['name'] in (manifest['induction_register'],carried_register):
            raise Unsupported('carried or terminal register remains in per-row shader contract')
        if node['op'] in ('entry_register','entry_vector') and node['name']!=manifest['induction_register']:
            for events in event_groups[:2]:
                first,last=events[0],events[-1]
                if node['op']=='entry_register':
                    k=GPRS.index(node['name']);stable=first['registers'][k]==last['registers'][k]
                else:
                    k=node['name'];stable=first['xmm_bytes'][k*16:k*16+16]==last['xmm_bytes'][k*16:k*16+16]
                if not stable:raise Unsupported('entry live-in not preserved across sampled path')
    expected=bytearray()
    for model in models[:2]:
        row=bytearray(words*4)
        for item in leaves:
            raw=bytes.fromhex(model['typed_ir']['nodes'][item['node']]['bytes'])
            at=item['slot']*4
            if at+len(raw)>len(row):raise Unsupported('shader input slot bounds')
            row[at:at+len(raw)]=raw
        expected.extend(row)
    required=[];visiting=set();done=set()
    def visit(node_id):
        if node_id in done:return
        if node_id in visiting:raise Unsupported('cyclic address DAG')
        visiting.add(node_id);node=nodes[node_id]
        if node['op']=='memory_input':
            if node_id not in origins:raise Unsupported('memory leaf lacks first original read')
            visit(origins[node_id])
        else:
            for inp in node['inputs']:visit(inp if isinstance(inp,int) else inp['id'])
        visiting.remove(node_id);done.add(node_id);required.append(node_id)
    for item in leaves:
        if nodes[item['node']]['op']!='entry_vector':visit(item['node'])
    def literal(raw):return '0x'+raw[::-1].hex()+'ULL'
    def mask(width):return '0xffffffffffffffffULL' if width==8 else hex((1<<(width*8))-1)+'ULL'
    statements=[];hoisted=[];template_writes=[];dynamic={}
    sorted_regions=sorted(manifest['regions'],key=lambda r:r['base'])
    for node_id in required:
        node=nodes[node_id];op=node['op'];width=len(bytes.fromhex(node['bytes']))
        if width>8:raise Unsupported('wide address computation')
        refs=[inp if isinstance(inp,int) else inp['id'] for inp in node['inputs']]
        dynamic[node_id]=(op=='entry_register' and node.get('name')==manifest['induction_register']) or any(dynamic[r] for r in refs)
        if op=='memory_input':dynamic[node_id]=dynamic[origins[node_id]]
        val=lambda i:f'v{refs[i]}'
        if op=='entry_register':
            base=int.from_bytes(bytes.fromhex(node['bytes']),'little')
            if node['name']==manifest['induction_register']:
                expr=f'({base}ULL + uint64_t(i)*{manifest["induction_step"]}ULL)'
            else:
                if any(other['typed_ir']['nodes'][node_id]['bytes']!=node['bytes'] for other in models[1:2]):
                    raise Unsupported('entry live-in varied across first two paths')
                expr=literal(bytes.fromhex(node['bytes']))
        elif op in ('immediate','constant','code_address'):
            expr=literal(bytes.fromhex(node['bytes']))
        elif op=='captured_teb':expr=f'{manifest["teb"]}ULL'
        elif op=='memory_input':
            if optimized:
                address=node['address']
                region_index=next((k for k,r in enumerate(sorted_regions)
                                   if r['base']<=address and address+width<=r['base']+r['size']),None)
                if region_index is None:raise Unsupported('leaf outside specialized region')
                expr=f'load_region({region_index},v{origins[node_id]},{width})'
            else:expr=f'load(v{origins[node_id]},{width})'
        elif op in ('address','lea'):
            recipe=node['recipe'];terms=[str(recipe['disp']&((1<<64)-1))+'ULL'];at=0
            if recipe.get('base'):terms.append(val(at));at+=1
            if recipe.get('index'):terms.append(f'({val(at)}*{recipe["scale"]}ULL)');at+=1
            if recipe.get('segment')=='gs':terms.append(val(at))
            elif recipe.get('segment'):raise Unsupported('segment recipe')
            expr='('+' + '.join(terms)+')'
        elif op in ('add','sub','shl'):
            expr=f'({val(0)} {"+" if op=="add" else "-" if op=="sub" else "<<"} {val(1)})'
        elif op=='stack_adjust':
            parent=nodes[refs[0]]
            delta=(int.from_bytes(bytes.fromhex(node['bytes']),'little')-
                   int.from_bytes(bytes.fromhex(parent['bytes']),'little'))&((1<<64)-1)
            if len(refs)!=1:raise Unsupported('stack adjustment arity')
            expr=f'({val(0)} + {delta}ULL)'
        elif op=='extract_bytes':expr=f'({val(0)} >> {8*node["byte_offset"]})'
        elif op=='pack_bytes':
            terms=[f'(((v{ref["id"]} >> {8*ref["byte_offset"]}) & 255ULL) << {8*j})'
                   for j,ref in enumerate(node['inputs'])]
            expr='('+' | '.join(terms)+')'
        else:raise Unsupported('native address operation '+op)
        line=f'    uint64_t v{node_id}=({expr}) & {mask(width)};'
        (statements if dynamic[node_id] or not optimized else hoisted).append(line)
    for item in leaves:
        node=nodes[item['node']];raw=bytes.fromhex(node['bytes']);at=item['slot']*4
        if node['op']=='entry_vector':
            if any(other['typed_ir']['nodes'][item['node']]['bytes']!=node['bytes'] for other in models[1:2]):
                raise Unsupported('entry vector changed across first two paths')
            if optimized:
                template_writes.append('  { const unsigned char literal[]={' + ','.join(str(b) for b in raw) +
                                       f'}}; memcpy(template_row.data()+{at},literal,{len(raw)}); }}')
            else:
                statements.append('    { const unsigned char literal[]={' + ','.join(str(b) for b in raw) +
                                  f'}}; memcpy(packed.data()+size_t(i)*{words*4}+{at},literal,{len(raw)}); }}')
        else:
            if optimized and not dynamic[item['node']]:
                template_writes.append(f'  memcpy(template_row.data()+{at},&v{item["node"]},{len(raw)});')
            else:
                statements.append(f'    memcpy(packed.data()+size_t(i)*{words*4}+{at},&v{item["node"]},{len(raw)});')
    view_rows=[]
    for item in manifest['regions']:
        path=str(Path(item['before_file']))
        if ')"' in path:raise Unsupported('unsafe path literal')
        view_rows.append(f'  views.push_back(load_view(LR"({path})",{item["base"]}ULL,{item["size"]}ULL));')
    expected_file=output_dir/'first-two-expected.bin'
    source=r'''// Generated from typed provenance; reads only cloned before-view files.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
struct View{uint64_t begin,end,lo=UINT64_MAX,hi=0;std::vector<char> bytes;};
static std::vector<char> file(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("open snapshot");auto n=f.tellg();if(n<0||n>128*1024*1024)throw std::runtime_error("snapshot budget");std::vector<char>b(size_t(n),0);f.seekg(0);f.read(b.data(),n);if(!f)throw std::runtime_error("read snapshot");return b;}
static View load_view(const wchar_t* path,uint64_t base,uint64_t size){auto b=file(path);if(b.size()!=size)throw std::runtime_error("view size mismatch");return {base,base+size,UINT64_MAX,0,std::move(b)};}
int wmain(int argc,wchar_t** argv){try{
  if(argc!=3){std::cerr<<"output.bin ranges.csv\n";return 2;}
  std::vector<View> views;
VIEW_ROWS
  std::sort(views.begin(),views.end(),[](const View&a,const View&b){return a.begin<b.begin;});
  for(size_t j=1;j<views.size();++j)if(views[j-1].end>views[j].begin)throw std::runtime_error("overlapping views");
  auto load=[&](uint64_t address,uint32_t size)->uint64_t{
    if(!size||size>8)throw std::runtime_error("load width");
    if(address<OUTPUT_END && address<=UINT64_MAX-size && address+size>OUTPUT_BEGIN)throw std::runtime_error("source aliases output reservation");
    for(auto& view:views)if(view.begin<=address&&address<=view.end&&size<=view.end-address){
      uint64_t result=0;memcpy(&result,view.bytes.data()+size_t(address-view.begin),size);
      view.lo=std::min(view.lo,address);view.hi=std::max(view.hi,address+size);return result;
    }
    throw std::runtime_error("unowned snapshot address");
  };
LOAD_REGION_FN
  std::vector<char> packed(uint64_t(COUNT)*ROW_BYTES,0);
  auto start=std::chrono::steady_clock::now();
HOISTED
TEMPLATE
  for(uint32_t i=0;i<COUNT;++i){
ROW_TEMPLATE_COPY
STATEMENTS
  }
  auto stop=std::chrono::steady_clock::now();
  auto expected=file(LR"(EXPECTED_PATH)");
  if(expected.size()!=2*ROW_BYTES||memcmp(packed.data(),expected.data(),expected.size()))throw std::runtime_error("first two traced rows mismatch");
  std::ofstream output(argv[1],std::ios::binary);output.write(packed.data(),packed.size());if(!output)throw std::runtime_error("write packed rows");
  std::ofstream ranges(argv[2]);ranges<<"begin,end\n";for(const auto& v:views)if(v.lo!=UINT64_MAX)ranges<<v.lo<<','<<v.hi<<'\n';
  if(!ranges)throw std::runtime_error("write ranges");
  std::cout<<"rows=COUNT row_bytes=ROW_BYTES gather_ms="<<std::chrono::duration<double,std::milli>(stop-start).count()<<" first_two_exact=1 bounded_snapshot_reads=1\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''
    source=source.replace('VIEW_ROWS','\n'.join(view_rows)).replace('STATEMENTS','\n'.join(statements))
    if optimized:
        region_fn='''  auto load_region=[&](size_t region,uint64_t address,uint32_t size)->uint64_t{
    if(region>=views.size()||!size||size>8)throw std::runtime_error("specialized region index/width");
    if(address<OUTPUT_END && address<=UINT64_MAX-size && address+size>OUTPUT_BEGIN)throw std::runtime_error("source aliases output reservation");
    auto& view=views[region];if(address<view.begin||address>view.end||size>view.end-address)throw std::runtime_error("specialized source region mismatch");
    uint64_t result=0;memcpy(&result,view.bytes.data()+size_t(address-view.begin),size);
    view.lo=std::min(view.lo,address);view.hi=std::max(view.hi,address+size);return result;
  };'''
        source=source.replace('LOAD_REGION_FN',region_fn)
        source=source.replace('HOISTED','\n'.join(hoisted))
        source=source.replace('ROW_TEMPLATE_COPY','    memcpy(packed.data()+size_t(i)*ROW_BYTES,template_row.data(),ROW_BYTES);')
        source=source.replace('TEMPLATE','  std::vector<char> template_row(ROW_BYTES,0);\n'+'\n'.join(template_writes))
    else:
        for token in ('LOAD_REGION_FN','HOISTED','ROW_TEMPLATE_COPY','TEMPLATE'):source=source.replace(token,'')
    source=source.replace('COUNT',str(manifest['packet_count'])).replace('ROW_BYTES',str(words*4))
    source=source.replace('OUTPUT_BEGIN',str(manifest['initial_cursor'])+'ULL')
    source=source.replace('OUTPUT_END',str(manifest['expected_final_cursor'])+'ULL')
    if separate_diagnostics:
        source=source.replace('  auto load=[&]', '  bool collect_ranges=true;\n  auto load=[&]')
        source=source.replace('    view.lo=std::min(view.lo,address);view.hi=std::max(view.hi,address+size);return result;',
                              '    if(collect_ranges){view.lo=std::min(view.lo,address);view.hi=std::max(view.hi,address+size);}return result;')
        start_line='  auto start=std::chrono::steady_clock::now();\n'
        stop_line='  auto stop=std::chrono::steady_clock::now();\n'
        if source.count(start_line)!=1 or source.count(stop_line)!=1:raise Unsupported('timer template changed')
        prefix,rest=source.split(start_line,1);body,suffix=rest.split(stop_line,1)
        wrapper=('  auto run=[&](){\n'+body+'  };\n'
                 '  auto verification_start=std::chrono::steady_clock::now();run();\n'
                 '  auto verification_stop=std::chrono::steady_clock::now();\n'
                 '  collect_ranges=false;\n'
                 '  auto start=std::chrono::steady_clock::now();run();\n'
                 '  auto stop=std::chrono::steady_clock::now();\n')
        source=prefix+wrapper+suffix
        source=source.replace('<<" first_two_exact=1 bounded_snapshot_reads=1\\n";',
            '<<" verification_ms="<<std::chrono::duration<double,std::milli>(verification_stop-verification_start).count()'
            '<<" full_prep_ms="<<std::chrono::duration<double,std::milli>((verification_stop-verification_start)+(stop-start)).count()'
            '<<" first_two_exact=1 bounded_snapshot_reads=1\\n";')
    source=source.replace('EXPECTED_PATH',str(expected_file.resolve()))
    output_dir.mkdir(parents=True,exist_ok=False)
    expected_file.write_bytes(expected)
    cpp=output_dir/'gather.cpp';cpp.write_text(source,encoding='utf-8')
    result={'schema':1,'status':'native_gather_source_generated','source':str(cpp.resolve()),
            'first_two_expected':str(expected_file.resolve()),'count':manifest['packet_count'],
            'row_bytes':words*4,'input_words_per_call':words,
            'shader_contract_sha256':hashlib.sha256(Path(contract_path).read_bytes()).hexdigest(),
            'native_manifest_sha256':hashlib.sha256(Path(manifest_path).read_bytes()).hexdigest(),
            'packet_kind':packet_kind,
            'leaf_nodes':[item['node'] for item in leaves],'bounds_checked_per_load':True,
            'shared_leaf_nodes':[item['node'] for item in leaves if nodes[item['node']]['op']=='entry_vector' or
                                 (item['node'] in dynamic and not dynamic[item['node']])],
            'per_row_leaf_nodes':[item['node'] for item in leaves if item['node'] in dynamic and dynamic[item['node']]],
            'snapshot_coherent_proven':False,'live_binding_allowed':False,
            'optimized_hoisted_nodes':len(hoisted) if optimized else 0,
            'optimized_specialized_bounded_loads':bool(optimized),
            'preflight_and_hoisting_inside_gather_ms':True,
            'diagnostics_outside_steady_timer':bool(separate_diagnostics),
            'full_per_batch_prep_includes_verification':bool(separate_diagnostics),
            'entry_live_in_scope':'non-induction register/vector values match entry and exit of first two sampled paths; invariance of all untraced paths not statically proven',
            'full_batch_inputs_generated':False,
            'compile_command':'cl /nologo /std:c++20 /EHsc /O2 /Fe:<exe> gather.cpp',
            'cursor_leaf_excluded':True,'terminal_bound_leaf_excluded':True}
    (output_dir/'generation.json').write_text(json.dumps(result,indent=2)+'\n')
    return result
