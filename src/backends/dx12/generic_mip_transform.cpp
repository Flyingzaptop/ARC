#include "generic_mip_transform.hpp"
#include <regex>
#include <sstream>
#include <vector>

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
MipTransform bias_pixel_mips(std::string_view input,unsigned half_steps){
    MipTransform result{std::string(input)};
    if(half_steps>8||input.size()>8*1024*1024||input.find("arc_pixel_mip_")!=input.npos)return result;
    std::smatch stage;const std::regex model(R"(!dx.shaderModel = !\{!(\d+)\})");
    if(!std::regex_search(result.ir,stage,model))return result;
    const std::regex pixel("!"+stage[1].str()+R"( = !\{!"ps", i32 [0-9]+, i32 [0-9]+\})");
    if(!std::regex_search(result.ir,pixel))return result;
    for(const char* op:{"@dx.op.textureStore","@dx.op.bufferStore","@dx.op.rawBufferStore","@dx.op.atomic","@dx.op.bufferUpdateCounter","@dx.op.createHandleFromHeap"})if(input.find(op)!=input.npos)return result;
    const std::regex call(R"(^([ \t]*%[A-Za-z0-9_.$]+ = call %dx.types.ResRet.f32 @dx.op.)(sample|sampleBias|sampleLevel)(\.f32\()(.*)(\)(?: #[0-9]+)?(?:, !.*)?)$)");
    std::ostringstream bias;bias.imbue(std::locale::classic());bias<<std::scientific;bias.precision(6);bias<<half_steps*.5;
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
                const auto id=std::to_string(result.samples++);
                if(!half_steps){output<<line<<'\n';continue;}
                if(kind=="sample"){args[0]="i32 61";args.insert(args.begin()+10,"float "+bias.str());kind="sampleBias";promoted=true;}
                else{
                    output<<"  %arc_pixel_mip_added"<<id<<" = fadd float "<<args[10].substr(6)<<", "<<bias.str()<<'\n';
                    if(kind=="sampleBias"){
                        output<<"  %arc_pixel_mip_limit"<<id<<" = fcmp ogt float %arc_pixel_mip_added"<<id<<", 1.500000e+01\n";
                        output<<"  %arc_pixel_mip_clamped"<<id<<" = select i1 %arc_pixel_mip_limit"<<id<<", float 1.500000e+01, float %arc_pixel_mip_added"<<id<<'\n';
                        args[10]="float %arc_pixel_mip_clamped"+id;
                    }else args[10]="float %arc_pixel_mip_added"+id;
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

}
