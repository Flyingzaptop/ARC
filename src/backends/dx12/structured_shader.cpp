#include "structured_shader.hpp"
#include <algorithm>
#include <charconv>
#include <climits>
#include <cctype>
#include <regex>
#include <sstream>

namespace arc::dx12::shader::structured {
namespace {
std::string trim(std::string_view value){const auto begin=value.find_first_not_of(" \t\r\n");
    if(begin==value.npos)return {};return std::string(value.substr(begin,value.find_last_not_of(" \t\r\n")-begin+1));}
std::string without_comment(std::string_view value){bool quoted=false,escape=false;for(std::size_t i=0;i<value.size();++i){
    const char ch=value[i];if(quoted){if(escape)escape=false;else if(ch=='\\')escape=true;else if(ch=='"')quoted=false;}
    else if(ch=='"')quoted=true;else if(ch==';')return trim(value.substr(0,i));}return trim(value);}
std::optional<unsigned> uint(std::string_view text){if(text=="-1")return UINT_MAX;unsigned value{};
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    if(error!=std::errc{}||end!=text.data()+text.size())return {};return value;}
std::optional<unsigned> typed_uint(std::string_view text,std::string_view type){if(!text.starts_with(type)||text.size()<=type.size()+1||text[type.size()]!=' ')return {};
    return uint(text.substr(type.size()+1));}
std::vector<std::string> split(std::string_view source){std::vector<std::string> result;unsigned depth{};bool quoted=false,escape=false;std::size_t begin{};
    for(std::size_t i=0;i<source.size();++i){const char c=source[i];if(quoted){if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quoted=false;continue;}
        if(c=='"'){quoted=true;continue;}if(c=='{'||c=='['||c=='('||c=='<')++depth;
        else if(c=='}'||c==']'||c==')'||c=='>'){if(!depth)return {};--depth;}
        else if(c==','&&!depth){result.push_back(trim(source.substr(begin,i-begin)));begin=i+1;}}
    if(depth||quoted)return {};result.push_back(trim(source.substr(begin)));return result;}
std::string final_ssa(std::string_view text){const std::regex ssa(R"(%[A-Za-z0-9_.$]+)");std::string result;const std::string source(text);
    for(auto it=std::sregex_iterator(source.begin(),source.end(),ssa);it!=std::sregex_iterator();++it)result=it->str();return result;}
std::vector<std::string> references(std::string_view text){const std::regex ssa(R"(%[A-Za-z0-9_.$]+)");std::vector<std::string> result;const std::string source(text);
    for(auto it=std::sregex_iterator(source.begin(),source.end(),ssa);it!=std::sregex_iterator();++it){const auto name=it->str();if(!name.starts_with("%dx.")&&!name.starts_with("%class."))result.push_back(name);}return result;}
std::string word(std::string_view source){const auto end=source.find(' ');return std::string(source.substr(0,end));}
std::optional<std::pair<unsigned,unsigned>> interval(const Program& program,std::string_view operand){
    if(const auto literal=typed_uint(operand,"i32"))return std::pair{*literal,*literal};
    if(const auto literal=uint(operand))return std::pair{*literal,*literal};
    const auto name=final_ssa(operand);if(const auto found=program.values.find(name);found!=program.values.end()&&found->second.finite_index)
        return std::pair{found->second.index_min,found->second.index_max};return {};
}
bool varying(const Program& program,std::string_view operand){const auto name=final_ssa(operand);
    if(name.empty())return false;const auto found=program.values.find(name);return found==program.values.end()||found->second.lane_varying;}
void derive_integer(Program& program,Value& value,const Operation& op,std::string_view rhs){
    if(op.callee=="dx.op.threadId.i32"){value.lane_varying=true;return;}
    if(op.callee=="dx.op.threadIdInGroup.i32"&&op.operands.size()==2){const auto axis=typed_uint(op.operands[1],"i32");
        if(axis&&*axis<3&&program.threads[*axis]){value.finite_index=true;value.index_max=program.threads[*axis]-1;value.lane_varying=true;}return;}
    if(op.callee=="dx.op.flattenedThreadIdInGroup.i32"){
        const std::uint64_t count=std::uint64_t(program.threads[0])*program.threads[1]*program.threads[2];
        if(count&&count<=UINT_MAX){value.finite_index=true;value.index_max=unsigned(count-1);value.lane_varying=true;}return;}
    const auto parts=split(rhs.substr(rhs.find(' ')+1));if(parts.size()!=2)return;
    const auto left=interval(program,parts[0]),right=interval(program,parts[1]);value.lane_varying=varying(program,parts[0])||varying(program,parts[1]);
    if(op.opcode=="and"){
        if(const auto mask=uint(parts[1])){value.finite_index=true;value.index_max=*mask;}
        else if(const auto mask=uint(parts[0])){value.finite_index=true;value.index_max=*mask;}}
    else if(op.opcode=="urem"){
        if(const auto divisor=uint(parts[1]);divisor&&*divisor){value.finite_index=true;value.index_max=*divisor-1;}}
    else if(op.opcode=="add"&&left&&right&&std::uint64_t(left->second)+right->second<=UINT_MAX){
        value.finite_index=true;value.index_min=left->first+right->first;value.index_max=left->second+right->second;}
    else if(op.opcode=="select"&&left&&right){value.finite_index=true;value.index_min=std::min(left->first,right->first);value.index_max=std::max(left->second,right->second);}
}
std::optional<std::vector<std::string>> call_arguments(std::string_view code,std::string& callee){
    const auto symbol=code.find('@');if(symbol==code.npos)return {};
    const auto begin=code.find('(',symbol);if(begin==code.npos)return {};
    const auto end=code.rfind(')');if(end==code.npos||end<=begin)return {};
    callee=std::string(code.substr(symbol+1,begin-symbol-1));return split(code.substr(begin+1,end-begin-1));
}
Effect call_effect(std::string_view name){
    if(name.starts_with("dx.op.textureStore")||name.starts_with("dx.op.bufferStore")||name.starts_with("dx.op.atomic"))return Effect::ResourceWrite;
    if(name.starts_with("dx.op.textureLoad")||name.starts_with("dx.op.sample")||name.starts_with("dx.op.bufferLoad")||
       name.starts_with("dx.op.rawBufferLoad")||name.starts_with("dx.op.cbufferLoad")||name=="dx.op.getDimensions")return Effect::ResourceRead;
    if(name=="dx.op.barrier")return Effect::Barrier;
    if(name=="dx.op.createHandleFromHeap"||name=="dx.op.createHandle"||name.starts_with("dx.op.rayQuery")||
       name.starts_with("dx.op.dispatch")||!name.starts_with("dx.op."))return Effect::Unknown;
    static const std::set<std::string> pure={"dx.op.createHandleFromBinding","dx.op.annotateHandle","dx.op.threadId.i32",
        "dx.op.threadIdInGroup.i32","dx.op.flattenedThreadIdInGroup.i32","dx.op.groupId.i32","dx.op.unary.f32",
        "dx.op.binary.f32","dx.op.tertiary.f32","dx.op.unary.i32","dx.op.binary.i32","dx.op.tertiary.i32",
        "dx.op.dot2.f32","dx.op.dot3.f32","dx.op.dot4.f32","dx.op.bitcastI32toF32","dx.op.bitcastF32toI32",
        "dx.op.isSpecialFloat.f32","dx.op.isSpecialFloat.f16","dx.op.legacyF16ToF32","dx.op.legacyF32ToF16"};
    return pure.contains(std::string(name))?Effect::Pure:Effect::Unknown;
}
std::optional<Binding> binding_call(const Program& program,const Operation& operation){const auto& args=operation.operands;
    if(args.size()!=4||args[0]!="i32 217"||!args[2].starts_with("i32 ")||(args[3]!="i1 false"&&args[3]!="i1 true"))return {};
    std::vector<std::string> fields;
    if(args[1]=="%dx.types.ResBind zeroinitializer")fields={"i32 0","i32 0","i32 0","i8 0"};
    else if(args[1].starts_with("%dx.types.ResBind {")&&args[1].ends_with('}')){
        const auto prefix=std::string_view("%dx.types.ResBind {");fields=split(std::string_view(args[1]).substr(prefix.size(),args[1].size()-prefix.size()-1));}
    if(fields.size()!=4)return {};
    const auto lower=typed_uint(fields[0],"i32"),upper=typed_uint(fields[1],"i32"),space=typed_uint(fields[2],"i32"),cls=typed_uint(fields[3],"i8");
    if(!lower||!upper||!space||!cls||*cls>3||*upper<*lower)return {};
    const auto resource=std::find_if(program.resources.begin(),program.resources.end(),[&](const auto& r){
        return r.resource_class==*cls&&r.shader_register==*lower&&r.space==*space&&r.count&&r.count<=64&&
            std::uint64_t(*lower)+r.count-1==*upper;});
    if(resource==program.resources.end())return {};
    const auto bounds=interval(program,args[2]);if(!bounds||bounds->first<*lower||bounds->second>*upper)return {};
    const bool dynamic=!final_ssa(args[2]).empty(),lane_varying=varying(program,args[2]);
    if(dynamic&&lane_varying&&args[3]!="i1 true")return {};
    return Binding{*cls,resource->range_id,*lower,*upper,*space,bounds->first,bounds->second,dynamic,args[3]=="i1 true"};
}
bool annotation(Program& program,const Operation& operation){const auto& args=operation.operands;
    if(args.size()!=3||args[0]!="i32 216"||!args[1].starts_with("%dx.types.Handle %")||
        !args[2].starts_with("%dx.types.ResourceProperties {")||!args[2].ends_with('}'))return false;
    const auto old=program.handles.find(final_ssa(args[1]));if(old==program.handles.end()||old->second.annotated)return false;
    const auto prefix=std::string_view("%dx.types.ResourceProperties {");const auto properties=split(
        std::string_view(args[2]).substr(prefix.size(),args[2].size()-prefix.size()-1));
    if(properties.size()!=2)return false;const auto first=typed_uint(properties[0],"i32"),second=typed_uint(properties[1],"i32");
    if(!first||!second)return false;auto binding=old->second;
    const auto resource=std::find_if(program.resources.begin(),program.resources.end(),[&](const auto& r){
        return r.resource_class==binding.resource_class&&r.range_id==binding.range_id;});
    if(resource==program.resources.end())return false;
    if(binding.resource_class==2){if(*first!=13||*second!=resource->kind)return false;}
    else if(binding.resource_class==3){if(*first!=14)return false;}
    else if((*first&0xff)!=resource->kind||((*first>>12)&1)!=(binding.resource_class==1))return false;
    binding.annotated=true;binding.properties0=*first;binding.properties1=*second;
    return program.handles.emplace(operation.result,binding).second;
}
bool resource_use(const Program& program,const Operation& operation){const auto& args=operation.operands;
    if(args.size()<2)return false;const auto h=program.binding_handle(final_ssa(args[1]));
    if(!h||!h->annotated)return false;
    const auto name=operation.callee;
    const unsigned expected=name.starts_with("dx.op.textureStore")||name.starts_with("dx.op.bufferStore")||name.starts_with("dx.op.atomic")?1:
        name.starts_with("dx.op.cbufferLoad")?2:0;
    if(name=="dx.op.getDimensions"){if(h->resource_class>1)return false;}
    else if(h->resource_class!=expected)return false;
    if(name.starts_with("dx.op.sample")){if(args.size()<3)return false;const auto sampler=program.binding_handle(final_ssa(args[2]));
        if(!sampler||!sampler->annotated||sampler->resource_class!=3)return false;}
    return true;
}
}

std::optional<Binding> Program::binding_handle(std::string_view name) const{
    const auto found=handles.find(std::string(name));if(found==handles.end())return {};return found->second;
}
Capability Program::capability(Action action) const{
    if(!structured_valid)return {false,reason.empty()?"structured_parse_unproven":reason,{}};
    if(action==Action::ExactReuse)return {false,"resource_versions_and_queue_order_unproven",{}};
    if(action==Action::LocalArithmetic)return {false,"local_arithmetic_rewrite_not_connected",{}};
    if(action==Action::SampleReduction)return {false,"sample_loop_semantics_not_proven_by_structured_path",{}};
    if(action==Action::ComparisonReduction)return {false,"comparison_footprint_not_proven_by_structured_path",{}};
    if(has_unknown_effects)return {false,"unknown_effect_or_heap_handle",{}};
    if(!finite_bindings_complete)return {false,"finite_binding_or_annotation_unproven",{}};
    if(!resource_writes)return {false,"no_pixel_outputs",{}};
    std::vector<Obligation> obligations;
    for(const auto& [name,handle]:handles)if(handle.dynamic&&handle.annotated)
        obligations.push_back({"all_declared_descriptors_at_dispatch",handle.resource_class,handle.range_id,handle.lower,handle.upper,handle.space});
    obligations.push_back({"legacy_local_memory_and_store_coverage_proof"});
    obligations.push_back({"physical_alias_and_volatility_guard_at_dispatch"});
    return {true,"structured_gate_passed_with_execution_obligations",std::move(obligations)};
}
Program analyze_dxil(std::string_view ir,const std::vector<ResourceContract>& resources,
    const std::array<unsigned,3>& threads,unsigned model_minor){
    Program program;program.shader_model_minor=model_minor;program.resources=resources;program.threads=threads;program.finite_bindings_complete=true;
    if(ir.empty()||ir.size()>8*1024*1024||model_minor!=6){program.reason="unsupported_structured_frontend_version";return program;}
    program.blocks.push_back({"entry"});bool inside=false,finished=false;unsigned functions{},line_number{};
    const std::regex label(R"(^; <label>:([0-9]+).*$)"),target(R"(label %([A-Za-z0-9_.$]+))");
    std::istringstream source{std::string(ir)};std::string line;
    while(std::getline(source,line)){++line_number;const auto raw=trim(line);
        if(raw.starts_with("define ")){inside=true;++functions;continue;}if(!inside)continue;
        if(raw=="}"){inside=false;finished=true;continue;}
        std::smatch match;if(std::regex_match(raw,match,label)){program.blocks.push_back({match[1].str()});continue;}
        const auto code=without_comment(raw);if(code.empty())continue;if(code.ends_with(':')){program.blocks.push_back({code.substr(0,code.size()-1)});continue;}
        Operation op;op.line=line_number;const auto assignment=code.find(" = ");const auto rhs=assignment==code.npos?code:code.substr(assignment+3);
        if(assignment!=code.npos)op.result=code.substr(0,assignment);
        op.opcode=word(rhs);if(op.opcode=="tail")op.opcode="call";
        if(op.opcode=="call"){
            const auto args=call_arguments(rhs,op.callee);if(!args){program.reason="structured_call_shape";return program;}
            op.operands=*args;op.effect=call_effect(op.callee);
            const auto at=rhs.find('@');const auto prefix=trim(std::string_view(rhs).substr(rhs.find("call")+4,at-rhs.find("call")-4));op.type=prefix;
        }else{
            op.operands=split(rhs.substr(std::min(rhs.size(),op.opcode.size()+1)));op.type=word(rhs.substr(std::min(rhs.size(),op.opcode.size()+1)));
            static const std::set<std::string> pure={"ret","br","switch","phi","add","sub","mul","and","or","xor","urem","udiv",
                "srem","sdiv","shl","lshr","ashr","icmp","fcmp","select","fadd","fsub","fmul","fdiv","frem","uitofp",
                "sitofp","fptoui","fptosi","zext","sext","trunc","bitcast","extractvalue","extractelement","insertelement",
                "shufflevector","fptrunc","fpext"};
            op.effect=pure.contains(op.opcode)?Effect::Pure:
                (op.opcode=="alloca"||op.opcode=="getelementptr"||op.opcode=="load"||op.opcode=="store")?Effect::PrivateMemory:Effect::Unknown;
        }
        if(op.effect==Effect::Unknown)program.has_unknown_effects=true;
        if(op.effect==Effect::Barrier)program.has_barrier=true;
        if(op.effect==Effect::ResourceRead)++program.resource_reads;
        if(op.effect==Effect::ResourceWrite)++program.resource_writes;
        if(!op.result.empty()){
            Value value;value.name=op.result;value.type=op.type;value.operation=op.opcode;value.block=program.blocks.back().label;
            value.operands=references(rhs);derive_integer(program,value,op,rhs);program.values.emplace(op.result,std::move(value));
        }
        if(op.callee=="dx.op.createHandleFromBinding"){
            if(op.result.empty())program.finite_bindings_complete=false;
            else if(auto binding=binding_call(program,op))program.handles.emplace(op.result,*binding);
            else program.finite_bindings_complete=false;
        }else if(op.callee=="dx.op.annotateHandle"){
            if(op.result.empty()||!annotation(program,op))program.finite_bindings_complete=false;
        }else if(op.effect==Effect::ResourceRead||op.effect==Effect::ResourceWrite){
            if(!resource_use(program,op))program.finite_bindings_complete=false;
        }
        if(op.opcode=="br"||op.opcode=="switch"){
            const auto& owner=program.blocks.back().label;
            for(auto it=std::sregex_iterator(code.begin(),code.end(),target);it!=std::sregex_iterator();++it)
                program.blocks.back().successors.insert((*it)[1].str());
            (void)owner;
        }
        program.blocks.back().operations.push_back(std::move(op));
    }
    if(functions!=1||!finished||program.blocks.empty()){program.reason="structured_entry_shape";return program;}
    // Conservative control dependence: every conditional predecessor's value
    // flows to reachable blocks. Over-approximation never proves uniformity.
    std::map<std::string,std::size_t> lookup;for(std::size_t i=0;i<program.blocks.size();++i)lookup[program.blocks[i].label]=i;
    for(auto& block:program.blocks)for(const auto& successor:block.successors){const auto found=lookup.find(successor);
        if(found==lookup.end()){program.reason="structured_branch_target";return program;}program.blocks[found->second].predecessors.insert(block.label);}
    for(std::size_t round=0;round<program.blocks.size();++round){bool changed=false;
        for(const auto& block:program.blocks){std::set<std::string> conditions=block.control_values;
            if(!block.operations.empty()){const auto& terminator=block.operations.back();
                if((terminator.opcode=="br"||terminator.opcode=="switch")&&block.successors.size()>1){
                    const auto id=final_ssa(terminator.operands.empty()?std::string{}:terminator.operands[0]);if(!id.empty())conditions.insert(id);}}
            for(const auto& successor:block.successors){auto& target_block=program.blocks[lookup.at(successor)];const auto before=target_block.control_values.size();
                target_block.control_values.insert(conditions.begin(),conditions.end());changed|=before!=target_block.control_values.size();}}
        if(!changed)break;}
    program.structured_valid=true;if(program.reason.empty())program.reason="structured_parsed";
    return program;
}
}
