#include "generic_pcf_transform.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <functional>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

namespace arc::dx12::shader {
namespace {
std::string trim(std::string s){const auto b=s.find_first_not_of(" \t\r\n");return b==s.npos?std::string{}:s.substr(b,s.find_last_not_of(" \t\r\n")-b+1);}
std::vector<std::string> fields(const std::string& s){std::vector<std::string> result;std::size_t start=0;for(std::size_t i=0;i<s.size();++i)if(s[i]==','){result.push_back(trim(s.substr(start,i-start)));start=i+1;}result.push_back(trim(s.substr(start)));return result;}
bool weight(const std::string& s,float& value){
    if(s.starts_with("0x")){std::uint64_t bits{};const auto [end,e]=std::from_chars(s.data()+2,s.data()+s.size(),bits,16);if(e!=std::errc{}||end!=s.data()+s.size())return false;value=static_cast<float>(std::bit_cast<double>(bits));return true;}
    const auto [end,e]=std::from_chars(s.data(),s.data()+s.size(),value);return e==std::errc{}&&end==s.data()+s.size();
}
std::string literal(float value){std::ostringstream out;out<<"0x"<<std::hex<<std::uppercase<<std::setfill('0')<<std::setw(16)<<std::bit_cast<std::uint64_t>(static_cast<double>(value));return out.str();}
struct Sample {unsigned line{},extract_line{};std::string result,scalar,block;std::vector<std::string> args;};
struct Sum {std::string a,b;};
struct Group {unsigned line{};std::string scale;std::vector<std::string> samples;};
}
ComparisonFilterTransform sparse_comparison_filter(std::string_view input){
    ComparisonFilterTransform result;result.ir=std::string(input);
    const std::string control=input.find("%arc_pixel_mip_handle =")!=input.npos?"%arc_pixel_mip_handle":"%arc_coarse_control";
    if(input.size()>8*1024*1024||input.find("arc_pcf_")!=input.npos||input.find(control+" =")==input.npos)return result;
    std::vector<std::string> lines,code,blocks;std::istringstream stream(result.ir);std::string line,current;
    while(std::getline(stream,line)){lines.push_back(line);const auto comment=line.find(';');const auto c=trim(line.substr(0,comment));code.push_back(c);if(c.ends_with(':'))current=c.substr(0,c.size()-1);blocks.push_back(current);}
    std::map<std::string,unsigned> references;const std::regex reference(R"(%[A-Za-z0-9_.$]+)");
    for(const auto& c:code)for(auto i=std::sregex_iterator(c.begin(),c.end(),reference);i!=std::sregex_iterator();++i)++references[i->str()];
    const std::string id=R"((%[A-Za-z0-9_.$]+))";
    const std::regex sample("^"+id+" = call %dx.types.ResRet.f32 @dx.op.sampleCmpLevelZero.f32\\((.*)\\)(?: #[0-9]+)?$");
    const std::regex extract("^"+id+" = extractvalue %dx.types.ResRet.f32 "+id+", 0$");
    const std::regex sum("^"+id+" = fadd(?: fast)? float "+id+", "+id+"$");
    const std::regex scale("^"+id+" = fmul(?: fast)? float "+id+", ([^ ]+)$");
    std::map<std::string,Sample> samples;std::map<std::string,Sum> sums;std::smatch m;
    for(unsigned i=0;i<code.size();++i){
        if(std::regex_match(code[i],m,sample)){
            Sample s;s.line=i;s.result=m[1];s.args=fields(m[2]);s.block=blocks[i];
            if(s.args.size()!=11||s.args[0]!="i32 65"||s.args[5]!="float undef"||s.args[6]!="float undef"||s.args[9]!="i32 undef"||references[s.result]!=2)continue;
            unsigned next=i+1;while(next<code.size()&&code[next].empty())++next;std::smatch e;
            if(next>=code.size()||!std::regex_match(code[next],e,extract)||e[2].str()!=s.result)continue;
            s.scalar=e[1];s.extract_line=next;if(references[s.scalar]!=2)continue;samples[s.scalar]=std::move(s);
        }else if(std::regex_match(code[i],m,sum))sums[m[1]]={m[2],m[3]};
    }
    std::vector<Group> groups;std::set<std::string> claimed;
    for(unsigned i=0;i<code.size();++i){if(!std::regex_match(code[i],m,scale))continue;float factor{};if(!weight(m[3],factor)||factor!=1.f/25.f)continue;
        Group group;group.line=i;group.scale=m[3];std::set<std::string> visited;
        std::function<bool(const std::string&)> visit=[&](const std::string& value){
            if(visited.size()>64||!visited.insert(value).second||references[value]!=2||claimed.contains(value))return false;
            if(samples.contains(value)){group.samples.push_back(value);return samples.at(value).block==blocks[i];}
            const auto it=sums.find(value);return it!=sums.end()&&visit(it->second.a)&&visit(it->second.b);
        };
        if(!visit(m[2])||group.samples.size()!=25)continue;
        std::vector<std::string> signature;std::set<std::pair<int,int>> offsets;bool valid=true;
        for(const auto& value:group.samples){auto args=samples.at(value).args;int dx{},dy{};auto parse=[](const std::string& text,int& v){if(!text.starts_with("i32 "))return false;const auto [end,e]=std::from_chars(text.data()+4,text.data()+text.size(),v);return e==std::errc{}&&end==text.data()+text.size();};
            if(!parse(args[7],dx)||!parse(args[8],dy)||dx<-2||dx>2||dy<-2||dy>2||!offsets.emplace(dx,dy).second){valid=false;break;}args[7]=args[8]="offset";
            if(signature.empty())signature=args;else if(signature!=args){valid=false;break;}
        }
        if(!valid||offsets.size()!=25)continue;
        for(const auto& value:visited)claimed.insert(value);groups.push_back(std::move(group));
    }
    if(groups.empty())return result;
    std::map<unsigned,std::string> replacements;std::set<unsigned> removed;
    unsigned serial{};
    for(const auto& group:groups){
        const auto name="%arc_pcf_weight"+std::to_string(result.groups);
        auto normalized=lines[group.line];const auto where=normalized.rfind(group.scale);if(where==normalized.npos)return ComparisonFilterTransform{std::string(input)};
        normalized.replace(where,group.scale.size(),name);
        replacements[group.line]="  "+name+" = select i1 %arc_pcf_enabled, float "+std::string("%arc_pcf_reciprocal")+", float "+group.scale+"\n"+normalized;
        for(const auto& scalar:group.samples){const auto& s=samples.at(scalar);int dx{},dy{};std::from_chars(s.args[7].data()+4,s.args[7].data()+s.args[7].size(),dx);std::from_chars(s.args[8].data()+4,s.args[8].data()+s.args[8].size(),dy);
            std::vector<std::pair<int,int>> order{{0,0},{-2,0},{2,0},{0,-2},{0,2},{-2,-2},{2,-2},{-2,2},{2,2}};
            std::vector<std::pair<int,int>> rest;for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x)if(std::find(order.begin(),order.end(),std::pair{x,y})==order.end())rest.emplace_back(x,y);
            std::stable_sort(rest.begin(),rest.end(),[](auto a,auto b){return a.first*a.first+a.second*a.second<b.first*b.first+b.second*b.second;});order.insert(order.end(),rest.begin(),rest.end());
            const auto rank=std::find(order.begin(),order.end(),std::pair{dx,dy})-order.begin();if(rank==0)continue;
            const auto suffix=std::to_string(serial++),full="arc_pcf_full"+suffix,skip="arc_pcf_skip"+suffix,merge="arc_pcf_merge"+suffix;
            auto call=code[s.line];call.replace(0,s.result.size(),"%arc_pcf_ret"+suffix);
            replacements[s.line]="  %arc_pcf_drop"+suffix+" = icmp ule i32 %arc_pcf_count, "+std::to_string(rank)+"\n  br i1 %arc_pcf_drop"+suffix+", label %"+skip+", label %"+full+"\n"+full+":\n  "+call+
                "\n  %arc_pcf_value"+suffix+" = extractvalue %dx.types.ResRet.f32 %arc_pcf_ret"+suffix+", 0\n  br label %"+merge+
                "\n"+skip+":\n  br label %"+merge+"\n"+merge+":\n  "+s.scalar+" = phi float [ %arc_pcf_value"+suffix+", %"+full+" ], [ 0.000000e+00, %"+skip+" ]";
            removed.insert(s.extract_line);++result.samples_removed;
        }
        ++result.groups;
    }
    std::ostringstream generated;std::map<std::string,std::string> predecessors;
    for(unsigned i=0;i<lines.size();++i){
        if(removed.contains(i))continue;
        if(auto it=replacements.find(i);it!=replacements.end()){
            generated<<it->second<<'\n';const auto position=it->second.rfind("\narc_pcf_merge");
            if(position!=it->second.npos){const auto end=it->second.find(':',position);predecessors[blocks[i]]=it->second.substr(position+1,end-position-1);}
        }else generated<<lines[i]<<'\n';
        if(code[i].starts_with(control+" ="))generated<<"  %arc_pcf_controls = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle "<<control<<", i32 1)\n"
            <<"  %arc_pcf_taps = extractvalue %dx.types.CBufRet.i32 %arc_pcf_controls, 0\n"
            <<"  %arc_pcf_positive = icmp uge i32 %arc_pcf_taps, 1\n"
            <<"  %arc_pcf_reduced = icmp ult i32 %arc_pcf_taps, 25\n"
            <<"  %arc_pcf_enabled = and i1 %arc_pcf_positive, %arc_pcf_reduced\n"
            <<"  %arc_pcf_count = select i1 %arc_pcf_enabled, i32 %arc_pcf_taps, i32 25\n"
            <<"  %arc_pcf_count_float = uitofp i32 %arc_pcf_count to float\n"
            <<"  %arc_pcf_reciprocal = fdiv float 1.000000e+00, %arc_pcf_count_float\n";
    }
    std::istringstream changed(generated.str());std::ostringstream fixed;
    while(std::getline(changed,line)){
        if(line.find(" = phi ")!=line.npos)for(const auto& [old_label,new_label]:predecessors){const std::regex incoming(",\\s*%"+old_label+"\\s*\\]");line=std::regex_replace(line,incoming,", %"+new_label+" ]");}
        fixed<<line<<'\n';}
    result.ir=fixed.str();return result;
}
}
