#include "generic_mip_transform.hpp"
#include <regex>
#include <sstream>
#include <vector>
#include <map>
#include <stdexcept>

namespace arc::dx12::shader {
MipTransform bias_explicit_mips(std::string_view input){
    MipTransform result{std::string(input)};
    if(input.size()>8*1024*1024||input.find("arc_mip_")!=input.npos||
       input.find("%arc_coarse_control =")==input.npos)return result;
    const std::regex call(R"(^([ \t]*%[A-Za-z0-9_.$]+ = call %dx.types.ResRet.f32 @dx.op.sampleLevel.f32\()(.*)(\)(?: #[0-9]+)?(?:, !.*)?)$)");
    std::istringstream source(result.ir);std::ostringstream output;std::string line;std::smatch match;
    while(std::getline(source,line)){
        auto code=line.substr(0,line.find(';'));const auto last=code.find_last_not_of(" \t\r");if(last!=code.npos)code.resize(last+1);
        if(std::regex_match(code,match,call)){
            std::vector<std::string> args;std::istringstream fields(match[2].str());std::string arg;
            while(std::getline(fields,arg,',')){const auto first=arg.find_first_not_of(" \t");args.push_back(first==arg.npos?"":arg.substr(first));}
            if(args.size()==11&&args[0]=="i32 62"&&args[10].starts_with("float ")&&args[10]!="float undef"){
                const auto id=std::to_string(result.samples++),lod=args[10].substr(6);
                const bool spatial=input.find("%arc_edge_allow =")!=input.npos;
                if(spatial)output<<"  %arc_mip_spatial"<<id<<" = and i1 %arc_mip_enabled, %arc_edge_allow\n";
                output<<"  %arc_mip_added"<<id<<" = fadd float "<<lod<<", %arc_mip_bias\n"
                      <<"  %arc_mip_lod"<<id<<" = select i1 "<<(spatial?"%arc_mip_spatial"+id:"%arc_mip_enabled")<<", float %arc_mip_added"<<id<<", float "<<lod<<"\n";
                args[10]="float %arc_mip_lod"+id;output<<match[1].str();
                for(unsigned i=0;i<args.size();++i){if(i)output<<", ";output<<args[i];}output<<match[3].str()<<'\n';continue;
            }
        }
        output<<line<<'\n';
        if(line.find("%arc_coarse_control =")!=line.npos){
            output<<"  %arc_mip_controls = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %arc_coarse_control, i32 2)\n"
                  <<"  %arc_mip_steps = extractvalue %dx.types.CBufRet.i32 %arc_mip_controls, 0\n"
                  <<"  %arc_mip_nonzero = icmp ugt i32 %arc_mip_steps, 0\n"
                  <<"  %arc_mip_bounded = icmp ule i32 %arc_mip_steps, 8\n"
                  <<"  %arc_mip_enabled = and i1 %arc_mip_nonzero, %arc_mip_bounded\n"
                  <<"  %arc_mip_safe_steps = select i1 %arc_mip_enabled, i32 %arc_mip_steps, i32 0\n"
                  <<"  %arc_mip_float = uitofp i32 %arc_mip_safe_steps to float\n"
                  <<"  %arc_mip_bias = fmul float %arc_mip_float, 5.000000e-01\n";
        }
    }
    if(result.samples)result.ir=output.str();return result;
}
static MipTransform pixel_mips_impl(std::string_view input,unsigned half_steps,bool controlled){
    MipTransform result{std::string(input)};
    if(half_steps>8||input.size()>8*1024*1024||input.find("arc_pixel_mip_")!=input.npos)return result;
    std::smatch stage;const std::regex model(R"(!dx.shaderModel = !\{!(\d+)\})");
    if(!std::regex_search(result.ir,stage,model))return result;
    const std::regex pixel("!"+stage[1].str()+R"( = !\{!"ps", i32 [0-9]+, i32 [0-9]+\})");
    if(!std::regex_search(result.ir,pixel))return result;
    for(const char* op:{"@dx.op.textureStore","@dx.op.bufferStore","@dx.op.rawBufferStore","@dx.op.atomic","@dx.op.bufferUpdateCounter","@dx.op.createHandleFromHeap"})if(input.find(op)!=input.npos)return result;
    const std::regex call(R"(^([ \t]*%[A-Za-z0-9_.$]+ = call %dx.types.ResRet.f32 @dx.op.)(sample|sampleBias|sampleLevel)(\.f32\()(.*)(\)(?: #[0-9]+)?(?:, !.*)?)$)");
    std::ostringstream bias;bias.imbue(std::locale::classic());bias<<std::scientific;bias.precision(6);bias<<half_steps*.5;const auto bias_value=controlled?std::string("%arc_pixel_mip_bias"):bias.str();
    std::istringstream source(result.ir);std::ostringstream output;std::string line,bias_declaration;std::smatch match;bool promoted=false;
    while(std::getline(source,line)){
        if(line.starts_with("declare %dx.types.ResRet.f32 @dx.op.sample.f32(")){
            bias_declaration=line;auto at=bias_declaration.find("sample.f32");bias_declaration.replace(at,10,"sampleBias.f32");bias_declaration.insert(bias_declaration.find(')'),", float");
        }
        auto code=line.substr(0,line.find(';'));auto end=code.find_last_not_of(" \t\r");if(end!=code.npos)code.resize(end+1);
        if(code.size()<65536&&std::regex_match(code,match,call)){
            auto kind=match[2].str();std::vector<std::string> args;std::istringstream fields(match[4].str());std::string arg;
            while(std::getline(fields,arg,',')){const auto first=arg.find_first_not_of(" \t");args.push_back(first==arg.npos?"":arg.substr(first));}
            const unsigned expected=kind=="sampleBias"?12:11;const auto opcode=kind=="sample"?"i32 60":kind=="sampleBias"?"i32 61":"i32 62";
            if(args.size()==expected&&args[0]==opcode&&(kind=="sample"||(args[10].starts_with("float ")&&args[10]!="float undef"))){
                const auto id=std::to_string(result.samples++);const auto original_kind=kind,original_lod=args[10];
                if(!half_steps&&!controlled){output<<line<<'\n';continue;}
                if(kind=="sample"){args[0]="i32 61";args.insert(args.begin()+10,"float "+bias_value);kind="sampleBias";promoted=true;}
                else{
                    output<<"  %arc_pixel_mip_added"<<id<<" = fadd float "<<args[10].substr(6)<<", "<<bias_value<<'\n';
                    if(kind=="sampleBias"){
                        output<<"  %arc_pixel_mip_limit"<<id<<" = fcmp ogt float %arc_pixel_mip_added"<<id<<", 1.500000e+01\n";
                        output<<"  %arc_pixel_mip_clamped"<<id<<" = select i1 %arc_pixel_mip_limit"<<id<<", float 1.500000e+01, float %arc_pixel_mip_added"<<id<<'\n';
                        args[10]="float %arc_pixel_mip_clamped"+id;
                    }else args[10]="float %arc_pixel_mip_added"+id;
                }
                if(controlled&&original_kind!="sample"){
                    output<<"  %arc_pixel_mip_selected"<<id<<" = select i1 %arc_pixel_mip_enabled, "<<args[10]<<", "<<original_lod<<'\n';args[10]="float %arc_pixel_mip_selected"+id;
                }
                output<<match[1].str()<<kind<<match[3].str();for(unsigned i=0;i<args.size();++i){if(i)output<<", ";output<<args[i];}output<<match[5].str()<<'\n';continue;
            }
        }
        output<<line<<'\n';
    }
    if(!result.samples||!half_steps)return result;
    if(promoted&&input.find("declare %dx.types.ResRet.f32 @dx.op.sampleBias.f32(")==input.npos){
        if(bias_declaration.empty()){result.samples=0;return result;}output<<'\n'<<bias_declaration<<'\n';
    }
    result.ir=output.str();
    const std::string old_name="@dx.op.sample.f32(";
    const auto first=result.ir.find(old_name);
    if(first!=std::string::npos&&result.ir.find(old_name,first+old_name.size())==std::string::npos){
        const auto begin=result.ir.rfind('\n',first);const auto line_begin=begin==std::string::npos?0:begin+1;
        if(result.ir.compare(line_begin,8,"declare ")==0){const auto end=result.ir.find('\n',first);result.ir.erase(line_begin,(end==std::string::npos?result.ir.size():end+1)-line_begin);}
    }
    return result;
}

MipTransform bias_pixel_mips(std::string_view input,unsigned steps){return pixel_mips_impl(input,steps,false);}
MipTransform controlled_pixel_mips(std::string_view input,unsigned space){
    MipTransform rejected{std::string(input)};
    if(space>=65536||input.find("@dx.op.createHandleFromBinding")!=input.npos)return rejected;
    auto result=pixel_mips_impl(input,2,true);if(!result.samples)return rejected;
    try{
        const auto split=[](const std::string& text){std::vector<std::string> out;std::size_t start=0;bool quote=false,escape=false;int depth=0;for(std::size_t i=0;i<text.size();++i){const char c=text[i];if(quote){if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quote=false;continue;}if(c=='"'){quote=true;continue;}if(c=='['||c=='{'||c=='('||c=='<')++depth;if(c==']'||c=='}'||c==')'||c=='>')--depth;if(c==','&&!depth){auto f=text.substr(start,i-start);const auto a=f.find_first_not_of(" \t");out.push_back(a==f.npos?"":f.substr(a));start=i+1;}}auto f=text.substr(start);const auto a=f.find_first_not_of(" \t");out.push_back(a==f.npos?"":f.substr(a));return out;};
        std::vector<std::string> lines;std::map<unsigned,std::vector<std::string>> metadata;std::map<unsigned,std::size_t> positions;unsigned root=UINT32_MAX,next=0,range=0;std::string line;std::smatch m;
        const std::regex node(R"(^!([0-9]+) = !\{(.*)\}$)"),resources(R"(^!dx.resources = !\{!([0-9]+)\}$)");std::istringstream source(result.ir);
        while(std::getline(source,line)){if(std::regex_match(line,m,node)){const auto id=unsigned(std::stoul(m[1]));if(id>=1000000)return rejected;next=std::max(next,id+1);metadata[id]=split(m[2]);positions[id]=lines.size();}if(std::regex_match(line,m,resources))root=unsigned(std::stoul(m[1]));lines.push_back(line);}
        if(!metadata.contains(root)||metadata[root].size()!=4)return rejected;auto lists=metadata[root];std::vector<std::string> cbuffers;
        if(lists[2]!="null"){cbuffers=metadata.at(unsigned(std::stoul(lists[2].substr(1))));for(const auto& ref:cbuffers){const auto& fields=metadata.at(unsigned(std::stoul(ref.substr(1))));if(fields.size()<7)return rejected;const auto id=std::stoul(fields[0].substr(4));if(id>=65535)return rejected;range=std::max(range,unsigned(id+1));if(fields[3]=="i32 "+std::to_string(space)&&fields[4]=="i32 0")return rejected;}}
        const auto entry=next++,list=next++;cbuffers.push_back("!"+std::to_string(entry));lists[2]="!"+std::to_string(list);
        const auto format=[](unsigned id,const std::vector<std::string>& fields){std::string out="!"+std::to_string(id)+" = !{";for(unsigned i=0;i<fields.size();++i){if(i)out+=", ";out+=fields[i];}return out+"}";};
        lines[positions.at(root)]=format(root,lists);std::ostringstream out;unsigned definitions=0;bool injected=false;
        out<<"%arc_pixel_control_buffer = type { [12 x i32] }\n";
        if(input.find("%dx.types.CBufRet.i32 = type")==input.npos)out<<"%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }\n";
        if(input.find("declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(")==input.npos)out<<"declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32)\n";
        for(const auto& l:lines){out<<l<<'\n';if(l.starts_with("define ")){if(++definitions>1||l.find("define void ")!=0||l.find("{")==l.npos)return rejected;
            out<<"  %arc_pixel_mip_handle = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 "<<range<<", i32 0, i1 false)\n"
               <<"  %arc_pixel_mip_values = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %arc_pixel_mip_handle, i32 2)\n"
               <<"  %arc_pixel_mip_steps = extractvalue %dx.types.CBufRet.i32 %arc_pixel_mip_values, 0\n"
               <<"  %arc_pixel_mip_safe = icmp ule i32 %arc_pixel_mip_steps, 8\n"
               <<"  %arc_pixel_mip_bounded = select i1 %arc_pixel_mip_safe, i32 %arc_pixel_mip_steps, i32 0\n"
               <<"  %arc_pixel_mip_enabled = icmp ne i32 %arc_pixel_mip_bounded, 0\n"
               <<"  %arc_pixel_mip_float = uitofp i32 %arc_pixel_mip_bounded to float\n"
               <<"  %arc_pixel_mip_bias = fmul float %arc_pixel_mip_float, 5.000000e-01\n";injected=true;
        }}
        if(!injected)return rejected;
        out<<format(list,cbuffers)<<"\n!"<<entry<<" = !{i32 "<<range<<", %arc_pixel_control_buffer* undef, !\"\", i32 "<<space<<", i32 0, i32 1, i32 48, null}\n";
        result.ir=out.str();return result;
    }catch(...){return rejected;}
}

}
