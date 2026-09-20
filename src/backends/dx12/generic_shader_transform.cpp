#include "generic_shader_transform.hpp"
#include <algorithm>
#include <charconv>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace arc::dx12::shader {
namespace {
std::string trim(std::string s) {
    const auto first=s.find_first_not_of(" \t\r\n"); if(first==s.npos)return {};
    return s.substr(first,s.find_last_not_of(" \t\r\n")-first+1);
}
unsigned number(std::string_view s) {
    unsigned value{};const auto [end,error]=std::from_chars(s.data(),s.data()+s.size(),value);
    if(error!=std::errc{}||end!=s.data()+s.size())throw std::runtime_error("integer");return value;
}
std::vector<std::string> fields(std::string_view s) {
    std::vector<std::string> out;unsigned nesting=0;bool quoted=false,escape=false;std::size_t start=0;
    for(std::size_t i=0;i<s.size();++i){const auto c=s[i];
        if(quoted){if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quoted=false;continue;}
        if(c=='"'){quoted=true;continue;}
        if(c=='['||c=='{'||c=='('||c=='<')++nesting;
        else if(c==']'||c=='}'||c==')'||c=='>'){if(!nesting)throw std::runtime_error("unbalanced");--nesting;}
        else if(c==','&&!nesting){out.push_back(trim(std::string(s.substr(start,i-start))));start=i+1;}
    }
    if(quoted||nesting)throw std::runtime_error("unbalanced");out.push_back(trim(std::string(s.substr(start))));return out;
}
unsigned integer(const std::string& s) {if(s=="i32 -1")return UINT32_MAX;if(!s.starts_with("i32 "))throw std::runtime_error("i32 expected");return number(std::string_view(s).substr(4));}
unsigned metadata_ref(const std::string& s) {if(s.empty()||s[0]!='!')throw std::runtime_error("metadata reference");return number(std::string_view(s).substr(1));}
std::string code(std::string line) {
    // Semicolons inside quoted identifiers/metadata are not comments.
    bool quote=false,escape=false;
    for(std::size_t i=0;i<line.size();++i){if(quote){if(escape)escape=false;else if(line[i]=='\\')escape=true;else if(line[i]=='"')quote=false;}
        else if(line[i]=='"')quote=true;else if(line[i]==';'){line.resize(i);break;}}
    return trim(line);
}
}
std::string normalize_converted_dxil(std::string_view input){
    if(input.empty()||input.size()>8*1024*1024)throw std::runtime_error("Converted IR size");
    std::string text(input);while(!text.empty()&&text.back()=='\0')text.pop_back();
    std::vector<std::string> lines;std::map<unsigned,std::vector<std::string>> nodes;std::map<unsigned,std::size_t> positions;
    unsigned resources=UINT_MAX;bool layout=false,triple=false;std::smatch match;
    const std::regex node(R"(^!([0-9]+) = !\{(.*)\}$)"),root(R"(^!dx.resources = !\{!([0-9]+)\}$)");
    std::istringstream source(text);std::string line;
    while(std::getline(source,line)){const auto clean=trim(line);
        if(std::regex_match(clean,match,node)){const auto id=number(match[1].str());nodes[id]=fields(match[2].str());positions[id]=lines.size();}
        else if(std::regex_match(clean,match,root))resources=number(match[1].str());
        layout|=clean.starts_with("target datalayout =");triple|=clean.starts_with("target triple =");lines.push_back(line);
    }
    if(resources==UINT_MAX||nodes.at(resources).size()!=4)throw std::runtime_error("Converted resource metadata");
    const std::regex pointer(R"(^(%[A-Za-z0-9_.$]+)( addrspace\([0-9]+\))?\* undef$)");
    for(const auto& list:nodes.at(resources)){if(list=="null")continue;for(const auto& reference:nodes.at(metadata_ref(list))){
        const auto id=metadata_ref(reference);auto& resource=nodes.at(id);if(resource.size()<7)throw std::runtime_error("Converted resource shape");
        const auto count=integer(resource[5]);if(count==1)continue;if(!count)throw std::runtime_error("Converted resource count");
        if(resource[1].starts_with('['))continue;
        if(!std::regex_match(resource[1],match,pointer))throw std::runtime_error("Converted resource type unsupported");
        const auto element=match[1].str(),space=match[2].str();resource[1]="["+std::to_string(count==UINT_MAX?0:count)+" x "+element+"]"+space+"* undef";
        std::string replacement="!"+std::to_string(id)+" = !{";for(unsigned i=0;i<resource.size();++i){if(i)replacement+=", ";replacement+=resource[i];}lines.at(positions.at(id))=replacement+"}";
    }}
    std::ostringstream output;
    if(!layout)output<<"target datalayout = \"e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64\"\n";
    if(!triple)output<<"target triple = \"dxil-ms-dx\"\n";
    const std::regex declaration(R"(^declare .*@([A-Za-z0-9_.$]+)\()" );
    for(const auto& original:lines){
        const auto clean=trim(original);
        if(std::regex_search(clean,match,declaration)){
            const auto symbol="@"+match[1].str();unsigned references=0;std::size_t cursor=0;
            while((cursor=text.find(symbol,cursor))!=text.npos){cursor+=symbol.size();const char next=cursor<text.size()?text[cursor]:'\0';
                const bool identifier=(next>='a'&&next<='z')||(next>='A'&&next<='Z')||(next>='0'&&next<='9')||next=='_'||next=='.'||next=='$';if(!identifier)++references;}
            if(references==1)continue; // Never remove an operation that is used.
        }
        output<<original<<'\n';
    }
    return output.str();
}

Transform coarse_compute(std::string_view input,unsigned x_rate,unsigned y_rate,bool runtime_control,unsigned requested_control_space) {
    Transform out;
    auto reject=[&](std::string reason){out.reason=std::move(reason);out.ir.clear();return out;};
    try {
        if(input.size()>8*1024*1024||input.empty())return reject("ir_size");
        if((x_rate!=1&&x_rate!=2)||(y_rate!=1&&y_rate!=2))return reject("rate");
        std::string text(input);while(!text.empty()&&text.back()=='\0')text.pop_back();
        if(text.find("arc_coarse_")!=text.npos)return reject("already_transformed");
        std::map<unsigned,std::vector<std::string>> metadata;
        std::map<std::string,unsigned> named;
        std::vector<std::string> lines;std::istringstream stream(text);std::string line;std::smatch m;
        const std::regex md(R"(^!([0-9]+) = !\{(.*)\}$)"), nm(R"(^!(dx\.[A-Za-z]+) = !\{!([0-9]+)\}$)");
        unsigned functions=0,next_metadata=0;std::size_t begin=0,end=0;
        const std::regex metadata_id(R"(^!([0-9]+) = )");
        while(std::getline(stream,line)){
            line=trim(line);
            std::smatch identity;if(std::regex_search(line,identity,metadata_id)){const auto id=number(identity[1].str());if(id>=1000000)return reject("metadata_capacity");next_metadata=std::max(next_metadata,id+1);}
            if(std::regex_match(line,m,md))metadata.emplace(number(m[1].str()),fields(m[2].str()));
            else if(std::regex_match(line,m,nm))named.emplace(m[1].str(),number(m[2].str()));
            if(line.starts_with("define ")){++functions;begin=lines.size();}
            if(line=="}")end=lines.size();
            lines.push_back(line);
        }
        if(functions!=1||end<=begin||!std::regex_match(lines[begin],std::regex(R"(^define void @[A-Za-z_.$][A-Za-z0-9_.$]*\(\) \{$)")))return reject("entry_shape");
        const auto& model=metadata.at(named.at("dx.shaderModel"));
        if(model.size()!=3||model[0]!="!\"cs\""||integer(model[1])!=6||integer(model[2])>5)return reject("shader_model");
        const auto& entries=metadata.at(named.at("dx.entryPoints"));
        if(entries.size()!=5)return reject("entry_metadata");
        const auto& properties=metadata.at(metadata_ref(entries[4]));
        if(properties.size()%2)return reject("entry_properties");
        for(std::size_t i=0;i<properties.size();i+=2){
            const auto tag=integer(properties[i]);
            if(tag==4){const auto& dims=metadata.at(metadata_ref(properties[i+1]));if(dims.size()!=3)return reject("thread_dimensions");for(unsigned j=0;j<3;++j)out.threads[j]=integer(dims[j]);}
            else if(tag!=0)return reject("unsupported_entry_property");
        }
        if(!out.threads[0]||!out.threads[1]||out.threads[2]!=1||out.threads[0]>1024||out.threads[1]>1024||out.threads[0]*out.threads[1]>1024)return reject("thread_dimensions");
        const auto& lists=metadata.at(named.at("dx.resources"));if(lists.size()!=4)return reject("resource_lists");
        for(unsigned cls=0;cls<4;++cls){if(lists[cls]=="null")continue;
            for(const auto& resource:metadata.at(metadata_ref(lists[cls]))){
                const auto& r=metadata.at(metadata_ref(resource));if(r.size()<7)return reject("resource_metadata");
                ResourceContract c{cls,integer(r[0]),integer(r[4]),integer(r[3]),integer(r[5]),integer(r[6])};
                if(!c.count||(c.count!=UINT32_MAX&&std::uint64_t(c.shader_register)+c.count>UINT32_MAX))return reject("resource_range");
                // A runtime submission must resolve a finite set of uniform
                // indices before any unbounded table can be activated.
                if(c.count==UINT32_MAX&&(!runtime_control||cls==2))return reject("unbounded_resource_requires_runtime_proof");
                if(cls==1&&(c.kind!=2||(!runtime_control&&c.count!=1)||r.size()!=11||r[7]!="i1 false"||r[8]!="i1 false"||r[9]!="i1 false"))return reject("uav_contract");
                if(std::any_of(out.resources.begin(),out.resources.end(),[&](const auto& old){return old.resource_class==cls&&old.range_id==c.range_id;}))return reject("duplicate_range");
                out.resources.push_back(c);
            }
        }
        struct Handle {unsigned cls{},range{};};std::map<std::string,Handle> handles;
        std::set<std::string> ray_queries;
        std::array<std::string,3> ids;std::array<std::set<std::string>,3> thread_ids;
        std::string entry_label="arc_coarse_orig_0";bool named_entry=false;
        for(std::size_t i=begin+1;i<end;++i){const auto first=code(lines[i]);if(first.empty())continue;if(first.ends_with(':')){entry_label=first.substr(0,first.size()-1);named_entry=true;}break;}
        struct Store {std::size_t line{};std::vector<std::string> args;};std::vector<Store> stores;
        const std::set<std::string> allowed_calls={"dx.op.createHandle","dx.op.threadId.i32","dx.op.textureLoad.f32","dx.op.textureLoad.i32",
            "dx.op.textureStore.f32","dx.op.cbufferLoadLegacy.f32","dx.op.cbufferLoadLegacy.i32","dx.op.sampleLevel.f32","dx.op.sampleCmpLevelZero.f32",
            "dx.op.unary.f32","dx.op.binary.f32","dx.op.tertiary.f32","dx.op.unary.i32","dx.op.binary.i32","dx.op.tertiary.i32",
            "dx.op.dot2.f32","dx.op.dot3.f32","dx.op.dot4.f32","dx.op.bitcastI32toF32","dx.op.bitcastF32toI32",
            "dx.op.legacyF16ToF32","dx.op.legacyF32ToF16","dx.op.getDimensions",
            "dx.op.binaryWithTwoOuts.i32",
            "dx.op.bufferLoad.f32","dx.op.bufferLoad.i32","dx.op.rawBufferLoad.f32","dx.op.rawBufferLoad.i32",
            "dx.op.allocateRayQuery","dx.op.rayQuery_TraceRayInline","dx.op.rayQuery_Proceed.i1","dx.op.rayQuery_StateScalar.i32"};
        const std::set<std::string> allowed_instructions={"call","ret","br","phi","add","sub","mul","udiv","sdiv","urem","srem","fadd","fsub","fmul","fdiv","frem",
            "shl","lshr","ashr","and","or","xor","icmp","fcmp","select","fptoui","fptosi","uitofp","sitofp","fptrunc","fpext","zext","sext","trunc","bitcast","extractvalue","extractelement","insertelement","shufflevector"};
        const std::regex call(R"(^(?:(%[A-Za-z0-9_.$]+) = )?call [^@]+@([A-Za-z0-9_.$]+)\((.*)\)(?: #[0-9]+)?$)");
        for(std::size_t i=begin+1;i<end;++i){
            auto c=code(lines[i]);if(c.empty()||c.ends_with(':'))continue;
            const auto eq=c.find(" = ");const auto instruction=c.substr(eq==c.npos?0:eq+3);const auto op=instruction.substr(0,instruction.find(' '));
            if(!allowed_instructions.contains(op))return reject("instruction:"+op);
            if(op!="call")continue;
            if(!std::regex_match(c,m,call))return reject("call_shape");
            const std::string name=m[2].str(),value=m[1].str();const auto args=fields(m[3].str());
            if(!allowed_calls.contains(name))return reject("call:"+name);
            if(name=="dx.op.createHandle"){
                if(args.size()!=5||args[1].size()<4||!args[1].starts_with("i8 "))return reject("handle_shape");
                const auto cls=number(std::string_view(args[1]).substr(3)),range=integer(args[2]);
                if(cls>3||std::none_of(out.resources.begin(),out.resources.end(),[&](const auto& r){return r.resource_class==cls&&r.range_id==range;}))return reject("handle_range");
                handles.emplace(value,Handle{cls,range});
                // Remapping virtual pixels can turn a group-uniform resource
                // index into a varying index. Preserve per-lane selection;
                // never retain a uniformity hint made false by the transform.
                if((runtime_control||x_rate>1||y_rate>1)&&args[3].starts_with("i32 %")&&args[4]=="i1 false"){
                    const auto flag=c.rfind("i1 false");if(flag==c.npos)return reject("handle_uniformity");lines[i].replace(flag,8,"i1 true");
                }
            } else if(name=="dx.op.allocateRayQuery"){
                if(args.size()!=2||args[0]!="i32 178"||value.empty()||ray_queries.size()>=16)return reject("ray_query_allocation");
                ray_queries.insert(value);
            } else if(name.starts_with("dx.op.rayQuery_")){
                if(args.size()<2||!args[1].starts_with("i32 ")||!ray_queries.contains(args[1].substr(4)))return reject("nonlocal_ray_query");
                if(name=="dx.op.rayQuery_TraceRayInline"){
                    if(args.size()!=13||args[0]!="i32 179")return reject("ray_trace_shape");
                    const auto h=args[2].substr(args[2].find_last_of(' ')+1);
                    if(!handles.contains(h)||handles.at(h).cls!=0)return reject("ray_trace_handle");
                    const auto range=handles.at(h).range;
                    if(std::none_of(out.resources.begin(),out.resources.end(),[&](const auto& r){return r.resource_class==0&&r.range_id==range&&r.kind==16;}))return reject("ray_trace_resource");
                }else if((name=="dx.op.rayQuery_Proceed.i1"&&(args.size()!=2||args[0]!="i32 180"))||
                         (name=="dx.op.rayQuery_StateScalar.i32"&&(args.size()!=2||args[0]!="i32 184")))return reject("ray_query_operation");
            } else if(name=="dx.op.threadId.i32"){
                if(args.size()!=2)return reject("thread_id");const auto dim=integer(args[1]);if(dim>1)return reject("thread_id");if(ids[dim].empty())ids[dim]=value;thread_ids[dim].insert(value);
            } else if(name=="dx.op.textureStore.f32"){
                if(args.size()!=10||args[4]!="i32 undef"||args[9]!="i8 15")return reject("store_shape");
                const auto h=args[1].substr(args[1].find_last_of(' ')+1);
                if(!handles.contains(h)||handles.at(h).cls!=1)return reject("store_handle");
                stores.push_back({i,args});
            } else if(name.starts_with("dx.op.textureLoad")||name.starts_with("dx.op.sample")||name.starts_with("dx.op.bufferLoad")||name.starts_with("dx.op.rawBufferLoad")){
                if(args.size()<2)return reject("read_shape");const auto h=args[1].substr(args[1].find_last_of(' ')+1);
                if(!handles.contains(h)||handles.at(h).cls!=0)return reject("uav_or_unknown_read");
            } else if(name=="dx.op.getDimensions"){
                if(args.size()!=3)return reject("dimension_shape");const auto h=args[1].substr(args[1].find_last_of(' ')+1);
                if(!handles.contains(h)||handles.at(h).cls>1)return reject("dimension_handle");
            }
        }
        if(ids[0].empty()||ids[1].empty()||stores.empty())return reject("no_pixel_outputs");
        for(const auto& s:stores)if(!s.args[2].starts_with("i32 ")||!s.args[3].starts_with("i32 ")||!thread_ids[0].contains(s.args[2].substr(4))||!thread_ids[1].contains(s.args[3].substr(4)))return reject("nonlocal_store");
        out.stores=static_cast<unsigned>(stores.size());
        unsigned control_range{};
        std::string appended_metadata;
        if(runtime_control){
            unsigned space=0;
            while(space<65536&&std::any_of(out.resources.begin(),out.resources.end(),[&](const auto& r){return r.space==space;}))++space;
            if(requested_control_space!=UINT32_MAX){space=requested_control_space;if(std::any_of(out.resources.begin(),out.resources.end(),[&](const auto& r){return r.space==space;}))return reject("control_space_collision");}
            if(space==65536)return reject("control_register_space");out.control_space=space;
            for(const auto& r:out.resources)if(r.resource_class==2){if(r.range_id==UINT32_MAX)return reject("control_range");control_range=std::max(control_range,r.range_id+1);}
            const auto root_id=named.at("dx.resources");auto resource_lists=metadata.at(root_id);
            unsigned next=next_metadata;if(next>1000000)return reject("metadata_capacity");
            const unsigned control_id=next++,list_id=next++;
            auto cbuffers=resource_lists[2]=="null"?std::vector<std::string>{}:metadata.at(metadata_ref(resource_lists[2]));
            cbuffers.push_back("!"+std::to_string(control_id));resource_lists[2]="!"+std::to_string(list_id);
            auto format=[](unsigned id,const std::vector<std::string>& f){std::string s="!"+std::to_string(id)+" = !{";for(unsigned j=0;j<f.size();++j){if(j)s+=", ";s+=f[j];}return s+"}";};
            for(auto& l:lines)if(l.starts_with("!"+std::to_string(root_id)+" = "))l=format(root_id,resource_lists);
            appended_metadata="\n"+format(list_id,cbuffers)+"\n!"+std::to_string(control_id)+" = !{i32 "+std::to_string(control_range)+
                ", %arc_coarse_control_buffer* undef, !\"\", i32 "+std::to_string(space)+", i32 0, i32 1, i32 48, null}\n";
        }
        // Numeric SSA ids and anonymous block ids must be named before adding
        // instructions. Renumbering only definitions would corrupt PHI edges.
        const std::regex local(R"(%([0-9]+)\b)"), label(R"(^; <label>:([0-9]+).*$)");
        for(std::size_t i=begin+1;i<end;++i){
            if(std::regex_match(lines[i],m,label))lines[i]="arc_coarse_orig_"+m[1].str()+":";
            else lines[i]=std::regex_replace(lines[i],local,"%arc_coarse_orig_$1");
        }
        std::ostringstream generated;
        const bool needs_bounds=runtime_control||x_rate>1||y_rate>1;
        if(needs_bounds&&!runtime_control){
            if(text.find("%dx.types.Dimensions = type")==text.npos)generated<<"%dx.types.Dimensions = type { i32, i32, i32, i32 }\n";
            if(text.find("declare %dx.types.Dimensions @dx.op.getDimensions(")==text.npos)generated<<"declare %dx.types.Dimensions @dx.op.getDimensions(i32, %dx.types.Handle, i32)\n";
        }
        if(runtime_control){
            generated<<"%arc_coarse_control_buffer = type { i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }\n";
            if(text.find("%dx.types.CBufRet.i32 = type")==text.npos)generated<<"%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }\n";
            if(text.find("declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(")==text.npos)
                generated<<"declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32)\n";
        }
        for(std::size_t i=0;i<lines.size();++i){
            generated<<lines[i]<<'\n';
            if(i==begin){
                generated<<"arc_coarse_entry:\n";
                if(runtime_control){
                    generated<<"  %arc_coarse_control = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 "<<control_range<<", i32 0, i1 false)\n"
                        <<"  %arc_coarse_values = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %arc_coarse_control, i32 0)\n"
                        <<"  %arc_coarse_width = extractvalue %dx.types.CBufRet.i32 %arc_coarse_values, 2\n"
                        <<"  %arc_coarse_height = extractvalue %dx.types.CBufRet.i32 %arc_coarse_values, 3\n";
                    for(unsigned d=0;d<2;++d){const char a=d?'y':'x';generated<<"  %arc_coarse_requested_"<<a<<" = extractvalue %dx.types.CBufRet.i32 %arc_coarse_values, "<<d<<"\n"
                        <<"  %arc_coarse_enabled_"<<a<<" = icmp eq i32 %arc_coarse_requested_"<<a<<", 2\n"
                        <<"  %arc_coarse_rate_"<<a<<" = select i1 %arc_coarse_enabled_"<<a<<", i32 2, i32 1\n";}
                }else if(needs_bounds){
                    const auto& first=stores.front().args[1];const auto handle=handles.at(first.substr(first.find_last_of(' ')+1));
                    const auto resource=std::find_if(out.resources.begin(),out.resources.end(),[&](const auto& r){return r.resource_class==1&&r.range_id==handle.range;});
                    if(resource==out.resources.end())return reject("output_extent_handle");
                    generated<<"  %arc_coarse_extent_handle = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 "<<handle.range<<", i32 "<<resource->shader_register<<", i1 false)\n"
                        <<"  %arc_coarse_dimensions = call %dx.types.Dimensions @dx.op.getDimensions(i32 72, %dx.types.Handle %arc_coarse_extent_handle, i32 undef)\n"
                        <<"  %arc_coarse_width = extractvalue %dx.types.Dimensions %arc_coarse_dimensions, 0\n"
                        <<"  %arc_coarse_height = extractvalue %dx.types.Dimensions %arc_coarse_dimensions, 1\n";
                }
                for(unsigned d=0;d<2;++d){const auto rate=d?y_rate:x_rate,group=out.threads[d];const char axis=d?'y':'x';
                    const auto rate_value=runtime_control?std::string("%arc_coarse_rate_")+axis:std::to_string(rate);
                    generated<<"  %arc_coarse_raw_"<<axis<<" = call i32 @dx.op.threadId.i32(i32 93, i32 "<<d<<")\n"
                        <<"  %arc_coarse_group_"<<axis<<" = udiv i32 %arc_coarse_raw_"<<axis<<", "<<group<<"\n"
                        <<"  %arc_coarse_parity_"<<axis<<" = urem i32 %arc_coarse_group_"<<axis<<", "<<rate_value<<"\n"
                        <<"  %arc_coarse_active_"<<axis<<" = icmp eq i32 %arc_coarse_parity_"<<axis<<", 0\n"
                        <<"  %arc_coarse_base_"<<axis<<" = mul i32 %arc_coarse_group_"<<axis<<", "<<group<<"\n"
                        <<"  %arc_coarse_local_"<<axis<<" = urem i32 %arc_coarse_raw_"<<axis<<", "<<group<<"\n"
                        <<"  %arc_coarse_scaled_"<<axis<<" = mul i32 %arc_coarse_local_"<<axis<<", "<<rate_value<<"\n"
                        <<"  %arc_coarse_coord_"<<axis<<" = add i32 %arc_coarse_base_"<<axis<<", %arc_coarse_scaled_"<<axis<<"\n";
                }
                generated<<"  %arc_coarse_group_active = and i1 %arc_coarse_active_x, %arc_coarse_active_y\n";
                if(needs_bounds){
                    generated<<"  %arc_coarse_inside_x = icmp ult i32 %arc_coarse_coord_x, %arc_coarse_width\n"
                        <<"  %arc_coarse_inside_y = icmp ult i32 %arc_coarse_coord_y, %arc_coarse_height\n"
                        <<"  %arc_coarse_inside = and i1 %arc_coarse_inside_x, %arc_coarse_inside_y\n";
                    if(runtime_control)generated<<"  %arc_coarse_is_coarse = or i1 %arc_coarse_enabled_x, %arc_coarse_enabled_y\n"
                        <<"  %arc_coarse_bounded = select i1 %arc_coarse_is_coarse, i1 %arc_coarse_inside, i1 true\n";
                    generated<<"  %arc_coarse_active = and i1 %arc_coarse_group_active, "<<(runtime_control?"%arc_coarse_bounded":"%arc_coarse_inside")<<"\n";
                }else generated<<"  %arc_coarse_active = and i1 %arc_coarse_group_active, true\n";
                generated<<"  br i1 %arc_coarse_active, label %"<<entry_label<<", label %arc_coarse_exit\n"
                    <<"arc_coarse_exit:\n  ret void\n";
                if(!named_entry)generated<<entry_label<<":\n";
            }
        }
        auto transformed=generated.str();
        // Replace original thread-id definitions, leaving the prelude calls
        // alone. All original instructions consequently use remapped pixels.
        for(unsigned d=0;d<2;++d)for(const auto& original_id:thread_ids[d]){
            const auto id=std::regex_replace(original_id,local,"%arc_coarse_orig_$1");
            for(std::size_t i=begin+1;i<end;++i){const auto c=code(lines[i]);if(!c.starts_with(id+" = call i32 @dx.op.threadId.i32("))continue;
                const auto pos=transformed.find(lines[i]);transformed.replace(pos,lines[i].size(),id+" = add i32 %arc_coarse_coord_"+(d?"y":"x")+", 0");}
        }
        unsigned serial{};std::size_t store_cursor{};
        std::map<std::string,std::string> predecessors;
        std::string current_block=entry_label;std::size_t block_cursor=begin+1;
        for(const auto& s:stores){
            for(;block_cursor<s.line;++block_cursor){const auto c=code(lines[block_cursor]);if(c.ends_with(':'))current_block=c.substr(0,c.size()-1);}
            const auto original=lines[s.line];const auto pos=transformed.find(original,store_cursor);if(pos==transformed.npos)return reject("store_rewrite");
            std::ostringstream expanded;expanded<<original<<'\n';
            for(unsigned y=0;y<(runtime_control?2:y_rate);++y)for(unsigned x=0;x<(runtime_control?2:x_rate);++x){if(!x&&!y)continue;
                auto a=s.args;for(auto& field:a)field=std::regex_replace(field,local,"%arc_coarse_orig_$1");
                const auto suffix=std::to_string(serial++);
                if(runtime_control){
                    expanded<<"  %arc_coarse_need_x"<<suffix<<" = icmp ugt i32 %arc_coarse_rate_x, "<<x<<"\n"
                        <<"  %arc_coarse_need_y"<<suffix<<" = icmp ugt i32 %arc_coarse_rate_y, "<<y<<"\n"
                        <<"  %arc_coarse_need"<<suffix<<" = and i1 %arc_coarse_need_x"<<suffix<<", %arc_coarse_need_y"<<suffix<<"\n"
                        <<"  br i1 %arc_coarse_need"<<suffix<<", label %arc_coarse_rep"<<suffix<<", label %arc_coarse_cont"<<suffix<<"\n"
                        <<"arc_coarse_rep"<<suffix<<":\n";
                }
                expanded<<"  %arc_coarse_store_x"<<suffix<<" = add i32 %arc_coarse_coord_x, "<<x<<"\n"
                    <<"  %arc_coarse_store_y"<<suffix<<" = add i32 %arc_coarse_coord_y, "<<y<<"\n";
                a[2]="i32 %arc_coarse_store_x"+suffix;a[3]="i32 %arc_coarse_store_y"+suffix;
                expanded<<"  call void @dx.op.textureStore.f32(";for(unsigned j=0;j<a.size();++j){if(j)expanded<<", ";expanded<<a[j];}expanded<<")\n";
                if(runtime_control){expanded<<"  br label %arc_coarse_cont"<<suffix<<"\narc_coarse_cont"<<suffix<<":\n";predecessors[current_block]="arc_coarse_cont"+suffix;}
            }
            const auto replacement=expanded.str();transformed.replace(pos,original.size(),replacement);store_cursor=pos+replacement.size();
        }
        if(runtime_control){
            std::istringstream modified(transformed);std::ostringstream fixed;
            while(std::getline(modified,line)){
                if(line.find(" = phi ")!=line.npos)for(const auto& [old_label,new_label]:predecessors){
                    const std::regex incoming(",\\s*%"+old_label+"\\s*\\]");line=std::regex_replace(line,incoming,", %"+new_label+" ]");
                }
                fixed<<line<<'\n';
            }
            transformed=fixed.str()+appended_metadata;
        }
        out.admitted=true;out.ir=std::move(transformed);out.reason="shader_local_only_requires_binding_and_quality_admission";return out;
    } catch(const std::exception&) {return reject("unsupported_or_malformed_ir");}
}
}
