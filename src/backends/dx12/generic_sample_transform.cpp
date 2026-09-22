#include "generic_sample_transform.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>
namespace arc::dx12::shader {
namespace {
std::string clean(std::string s){if(auto at=s.find(';');at!=s.npos)s.resize(at);auto a=s.find_first_not_of(" \t\r\n");return a==s.npos?"":s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);}
float constant(const std::string& s){return s.starts_with("0x")?float(std::bit_cast<double>(std::stoull(s.substr(2),nullptr,16))):std::stof(s);}
}
static SampleTransform reduce_means(std::string_view source,bool rays){
    SampleTransform result{std::string(source),0};
    if((!rays&&source.find("rayQuery")!=source.npos)||source.find("addrspace(3)")!=source.npos)return result;
    try{
        std::vector<std::string> lines,owner;std::map<std::string,std::size_t> definition;std::map<std::string,std::vector<std::size_t>> blocks;std::map<std::string,std::set<std::string>> predecessors;
        std::istringstream input{std::string(source)};std::string line,block,control_name;std::smatch m;unsigned range=~0u;
        const std::regex cb(R"(^(%arc_coarse_control|%arc_pixel_mip_handle) = call %dx.types.Handle @dx.op.createHandle\(i32 57, i8 2, i32 ([0-9]+), i32 0, i1 false\).*$)");
        while(std::getline(input,line)){const auto c=clean(line);if(c.ends_with(':'))block=c.substr(0,c.size()-1);if(std::regex_match(c,m,cb)){control_name=m[1];range=std::stoul(m[2]);}auto eq=c.find(" = ");if(eq!=c.npos&&c.starts_with('%'))definition[c.substr(0,eq)]=lines.size();blocks[block].push_back(lines.size());owner.push_back(block);lines.push_back(line);}
        if(range==~0u)return result;std::vector<std::string> codes;for(const auto& l:lines)codes.push_back(clean(l));
        for(auto& [name,indices]:blocks)std::erase_if(indices,[&](auto at){return codes[at].empty();});
        const std::regex phi(R"(^(%[A-Za-z0-9_.$]+) = phi (i32|float) \[ ([^,]+), %([A-Za-z0-9_.$]+) \], \[ ([^,]+), %([A-Za-z0-9_.$]+) \].*$)");
        const std::regex inc(R"(^(%[A-Za-z0-9_.$]+) = add(?: nuw| nsw)* i32 (%[A-Za-z0-9_.$]+), 1$)");
        const std::regex cmp(R"(^(%[A-Za-z0-9_.$]+) = icmp (eq|ult|ne) i32 (%[A-Za-z0-9_.$]+), ([0-9]+)$)");
        const std::regex branch(R"(^br i1 (%[A-Za-z0-9_.$]+), label %([A-Za-z0-9_.$]+), label %([A-Za-z0-9_.$]+)(?:, !.*)?$)");
        const std::regex add(R"(^(%[A-Za-z0-9_.$]+) = fadd(?: fast)? float ([^,]+), (.+)$)");
        const std::regex mul(R"(^(%[A-Za-z0-9_.$]+) = fmul(?: fast)? float ([^,]+), (.+)$)");
        const std::regex ref(R"(%[A-Za-z0-9_.$]+)"),edge(R"(label %([A-Za-z0-9_.$]+))");
        for(std::size_t at=0;at<codes.size();++at)if(codes[at].starts_with("br ")||codes[at].starts_with("switch "))for(auto e=std::sregex_iterator(codes[at].begin(),codes[at].end(),edge);e!=std::sregex_iterator();++e)predecessors[(*e)[1]].insert(owner[at]);
        for(const auto& [counter,index]:definition){
            const auto header=owner[index];if(!std::regex_match(codes[index],m,phi)||m[2]!="i32")continue;
            auto initial=m[3].str(),pre=m[4].str(),back=m[5].str(),latch=m[6].str();if(initial!="0"){std::swap(initial,back);std::swap(pre,latch);}if(initial!="0"||(!rays&&latch!=header)||pre==header||!definition.contains(back))continue;
            if(!std::regex_match(codes[definition[back]],m,inc)||m[2]!=counter||owner[definition[back]]!=latch)continue;
            std::size_t branch_at=lines.size(),comparison_at=lines.size();unsigned count=0;
            for(auto at:blocks[latch])if(std::regex_match(codes[at],m,branch)){const auto test=m[1].str(),yes=m[2].str(),no=m[3].str();if(!definition.contains(test))continue;const auto check=definition[test];
                if(!std::regex_match(codes[check],m,cmp)||m[3]!=back)continue;const auto op=m[2].str();count=std::stoul(m[4]);if(count<4||count>256)continue;
                if((op=="eq"&&no==header&&yes!=header)||(op!="eq"&&yes==header&&no!=header)){branch_at=at;comparison_at=check;break;}}
            if(branch_at==lines.size())continue;
            std::set<std::string> region{header};std::vector<std::size_t> region_lines=blocks[header];
            if(rays){
                if(latch==header||!blocks.contains(latch))continue;
                const std::regex jump(R"(^br label %([A-Za-z0-9_.$]+)(?:, !.*)?$)");std::smatch j;
                if(!std::regex_match(codes[blocks[header].back()],j,jump))continue;const auto inner=j[1].str();
                if(inner==header||inner==latch||!blocks.contains(inner))continue;
                if(predecessors[header]!=std::set<std::string>{pre,latch}||predecessors[inner]!=std::set<std::string>{header,inner}||predecessors[latch]!=std::set<std::string>{inner})continue;
                std::string handle,proceed;unsigned allocation_count=0,trace_count=0,proceed_count=0,status_count=0;bool proof=true;
                const std::regex allocation(R"(^(%[A-Za-z0-9_.$]+) = call i32 @dx.op.allocateRayQuery\(i32 178, i32 [0-9]+\).*$)");
                for(auto at:blocks[header]){std::smatch a;if(std::regex_match(codes[at],a,allocation)){handle=a[1];++allocation_count;}}
                if(allocation_count!=1)continue;
                for(auto at:blocks[header])if(codes[at].find("@dx.op.rayQuery_TraceRayInline(i32 179, i32 "+handle+",")!=std::string::npos)++trace_count;
                if(trace_count!=1)continue;
                for(auto at:blocks[inner]){const auto& c=codes[at];if(c.empty()||c==inner+":")continue;
                    const auto marker=" = call i1 @dx.op.rayQuery_Proceed.i1(i32 180, i32 "+handle+")";
                    if(auto offset=c.find(marker);offset!=std::string::npos){proceed=c.substr(0,offset);++proceed_count;}
                    else if(!c.starts_with("br i1 "))proof=false;
                }
                std::smatch inner_branch;if(proceed_count!=1||!std::regex_match(codes[blocks[inner].back()],inner_branch,branch)||inner_branch[1]!=proceed||inner_branch[2]!=inner||inner_branch[3]!=latch)continue;
                region.insert(inner);region.insert(latch);region_lines.insert(region_lines.end(),blocks[inner].begin(),blocks[inner].end());region_lines.insert(region_lines.end(),blocks[latch].begin(),blocks[latch].end());
                for(auto at:region_lines){const auto& c=codes[at];
                    if(c.find("@dx.op.rayQuery_StateScalar.i32(i32 184, i32 "+handle+")")!=std::string::npos)++status_count;
                    if(c.find("call ")!=c.npos&&c.find("@dx.op.")==c.npos)proof=false;
                    if(c.find("@dx.op.")!=c.npos){const auto start=c.find("@dx.op.")+7,end=c.find('(',start);const auto op=c.substr(start,end-start);
                        if(!op.starts_with("unary.")&&!op.starts_with("binary.")&&!op.starts_with("dot")&&!op.starts_with("tertiary.")&&op!="allocateRayQuery"&&op!="rayQuery_TraceRayInline"&&op!="rayQuery_Proceed.i1"&&op!="rayQuery_StateScalar.i32")proof=false;}
                    if(c.starts_with("store ")||c.find("atomic")!=c.npos||c.find(" = phi ")!=c.npos&&owner[at]!=header)proof=false;
                    if(c.find(handle)!=c.npos&&c.find("allocateRayQuery")==c.npos&&c.find("rayQuery_")==c.npos)proof=false;
                }
                // No ray state or side result can escape this independently reset region.
                for(std::size_t at=0;at<codes.size();++at)if(!region.contains(owner[at])&&codes[at].find(handle)!=std::string::npos)proof=false;
                if(!proof||status_count!=1)continue;
            }
            std::set<std::string> accumulators,sums;std::map<std::string,std::size_t> accumulator_add;bool valid=true,has_samples=rays;
            for(auto at:blocks[header]){const auto c=codes[at];if(c.find("@dx.op.sampleLevel.f32(")!=c.npos||c.find("@dx.op.sampleBias.f32(")!=c.npos||c.find("@dx.op.sample.f32(")!=c.npos||c.find("@dx.op.textureLoad.f32(")!=c.npos)has_samples=true;
                if(c.starts_with("store ")||c.find("textureStore")!=c.npos||c.find("bufferStore")!=c.npos||c.find("atomic")!=c.npos||c.find("@dx.op.storeOutput")!=c.npos||c.find("@dx.op.discard")!=c.npos||c.starts_with("br ")&&!rays&&at!=branch_at)valid=false;
                if(c.find(" = phi ")!=c.npos&&at!=index){if(!std::regex_match(c,m,phi)||m[2]!="float"){valid=false;break;}const auto id=m[1].str();auto init=m[3].str(),a=m[4].str(),sum=m[5].str(),b=m[6].str();if(a==latch){std::swap(init,sum);std::swap(a,b);}if(a!=pre||b!=latch||init.starts_with('%')||constant(init)!=0||!definition.contains(sum)){valid=false;break;}
                    const auto added=definition[sum];if(owner[added]!=latch||!std::regex_match(codes[added],m,add)||(m[2]!=id&&m[3]!=id)){valid=false;break;}accumulators.insert(id);sums.insert(sum);accumulator_add[id]=added;}}
            if(!valid||!has_samples||sums.empty()||sums.size()>4)continue;
            std::map<std::size_t,std::string> normalizations;std::set<std::string> normalized;
            for(std::size_t at=0;at<lines.size()&&valid;++at){const auto c=codes[at];const auto eq=c.find(" = ");const auto uses=eq==c.npos?c:c.substr(eq+3);
                for(auto it=std::sregex_iterator(uses.begin(),uses.end(),ref);it!=std::sregex_iterator();++it){const auto id=it->str();if(accumulators.contains(id)&&at!=accumulator_add[id]){valid=false;break;}
                    if(!definition.contains(id)||!region.contains(owner[definition[id]])||region.contains(owner[at]))continue;
                    if(!sums.contains(id)||!std::regex_match(c,m,mul)){valid=false;break;}const auto other=m[2]==id?m[3].str():m[2].str();if(other.starts_with('%')||constant(other)!=float(1.f/count)){valid=false;break;}normalizations[at]=m[1];normalized.insert(id);}}
            if(!valid||normalized!=sums)continue;
            const auto tag=std::to_string(result.loops);const auto prefix=std::string(rays?"%arc_rays_":"%arc_samples_")+tag;
            std::ostringstream controls;controls<<"  "<<prefix<<"_cb = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 "<<range<<", i32 0, i1 false)\n"
                <<"  "<<prefix<<"_data = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle "<<prefix<<"_cb, i32 2)\n"
                <<"  "<<prefix<<"_percent = extractvalue %dx.types.CBufRet.i32 "<<prefix<<"_data, 1\n";
            for(unsigned p:{25u,50u,75u})controls<<"  "<<prefix<<"_is"<<p<<" = icmp eq i32 "<<prefix<<"_percent, "<<p<<"\n";
            controls<<"  "<<prefix<<"_count25 = select i1 "<<prefix<<"_is25, i32 "<<(count+3)/4<<", i32 "<<count<<"\n"
                <<"  "<<prefix<<"_count50 = select i1 "<<prefix<<"_is50, i32 "<<(count+1)/2<<", i32 "<<prefix<<"_count25\n"
                <<"  "<<prefix<<"_count = select i1 "<<prefix<<"_is75, i32 "<<(count*3+3)/4<<", i32 "<<prefix<<"_count50\n"
                <<"  "<<prefix<<"_float = uitofp i32 "<<prefix<<"_count to float\n"
                <<"  "<<prefix<<"_factor = fdiv float "<<std::scientific<<float(count)<<", "<<prefix<<"_float\n"
                <<"  "<<prefix<<"_neutral = icmp eq i32 "<<prefix<<"_count, "<<count<<"\n";
            // Every original loop is dominated by the inserted coarse entry.
            for(auto& l:lines)if(clean(l).starts_with(control_name+" =")){l+='\n'+controls.str();break;}
            auto& comparison=lines[comparison_at];const auto last=comparison.find_last_of(',');comparison=comparison.substr(0,last+1)+" "+prefix+"_count";
            for(const auto& [at,id]:normalizations){auto original=lines[at];const auto start=original.find(id);original.replace(start,id.size(),prefix+"_original"+std::to_string(at));const auto value=prefix+"_original"+std::to_string(at),adjusted=prefix+"_adjusted"+std::to_string(at);
                lines[at]=original+"\n  "+adjusted+" = fmul float "+value+", "+prefix+"_factor\n  "+id+" = select i1 "+prefix+"_neutral, float "+value+", float "+adjusted;}
            ++result.loops;break; // one independently proved mutation per shader
        }
        if(result.loops){std::ostringstream text;for(const auto& l:lines)text<<l<<'\n';result.ir=text.str();}
    }catch(...){return {std::string(source),0};}
    return result;
}
SampleTransform reduce_sample_means(std::string_view source){return reduce_means(source,false);}
SampleTransform reduce_ray_means(std::string_view source){return reduce_means(source,true);}

}
