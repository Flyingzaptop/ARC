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
}
