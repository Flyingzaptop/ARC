#include "generic_zero_transform.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

namespace arc::dx12::shader {
namespace {
std::string trim(std::string s){auto b=s.find_first_not_of(" \t\r\n");return b==s.npos?std::string{}:s.substr(b,s.find_last_not_of(" \t\r\n")-b+1);}
std::optional<float> number(const std::string& s){float f{};if(s.starts_with("0x")){std::uint64_t bits{};auto [end,e]=std::from_chars(s.data()+2,s.data()+s.size(),bits,16);if(e!=std::errc{}||end!=s.data()+s.size())return {};f=static_cast<float>(std::bit_cast<double>(bits));}else{auto [end,e]=std::from_chars(s.data(),s.data()+s.size(),f);if(e!=std::errc{}||end!=s.data()+s.size())return {};}return std::isfinite(f)?std::optional(f):std::nullopt;}
struct Block {std::string label,condition;unsigned begin{},end{};std::vector<unsigned> next,pred;};
struct Definition {unsigned line{},block{};};
struct ZeroSet {bool all{};std::set<std::string> values;};
ZeroSet unite(ZeroSet a,const ZeroSet& b){if(a.all||b.all)return {true,{}};a.values.insert(b.values.begin(),b.values.end());return a;}
ZeroSet intersect(const ZeroSet& a,const ZeroSet& b){if(a.all)return b;if(b.all)return a;ZeroSet r;std::set_intersection(a.values.begin(),a.values.end(),b.values.begin(),b.values.end(),std::inserter(r.values,r.values.end()));return r;}
struct Candidate {unsigned start{},end{},split{},score{};bool zero_prone{};std::string factor;std::set<unsigned> region;std::vector<std::string> outputs;};
}
ZeroFactorTransform short_circuit_zero_factors(std::string_view input){
    ZeroFactorTransform result;result.ir=std::string(input);
    if(input.size()>8*1024*1024||input.find("arc_zero_")!=input.npos||input.find("%arc_coarse_control =")==input.npos)return result;
    try{
        std::vector<std::string> lines,code;std::vector<Block> blocks;std::map<std::string,unsigned> labels;
        std::map<std::string,Definition> definitions;std::map<std::string,std::set<unsigned>> users;
        std::map<unsigned,std::vector<std::string>> line_refs;
        const std::regex ref(R"(%[A-Za-z0-9_.$]+)"),def(R"(^(%[A-Za-z0-9_.$]+) = .*$)"),label(R"(^([A-Za-z0-9_.$]+):.*$)");
        const std::regex jump(R"(^br label %([A-Za-z0-9_.$]+)(?:, !.*)?$)"),branch(R"(^br i1 (%[A-Za-z0-9_.$]+|true|false), label %([A-Za-z0-9_.$]+), label %([A-Za-z0-9_.$]+)(?:, !.*)?$)");
        std::istringstream source(result.ir);std::string line;std::smatch m;bool inside=false;unsigned current=UINT32_MAX;
        while(std::getline(source,line)){
            const unsigned index=static_cast<unsigned>(lines.size());lines.push_back(line);const auto comment=line.find(';');auto c=trim(line.substr(0,comment));code.push_back(c);
            if(c.starts_with("define ")){if(inside)return result;inside=true;continue;}if(!inside)continue;
            if(c=="}"){if(current!=UINT32_MAX)blocks[current].end=index;inside=false;continue;}
            if(std::regex_match(c,m,label)){if(current!=UINT32_MAX)blocks[current].end=index;current=static_cast<unsigned>(blocks.size());labels[m[1]]=current;blocks.push_back({m[1],{},index+1,index+1});continue;}
            if(current==UINT32_MAX||c.empty())continue;
            std::string output;if(std::regex_match(c,m,def)){output=m[1];definitions[output]={index,current};}
            for(auto it=std::sregex_iterator(c.begin(),c.end(),ref);it!=std::sregex_iterator();++it){const auto value=it->str();if(value==output)continue;users[value].insert(current);line_refs[index].push_back(value);}
        }
        if(blocks.empty()||blocks.size()>2048)return result;
        for(unsigned i=0;i<blocks.size();++i){auto& b=blocks[i];unsigned last=b.end;while(last>b.begin&&code[last-1].empty())--last;if(last==b.begin)return result;const auto& terminator=code[last-1];
            if(std::regex_match(terminator,m,jump))b.next.push_back(labels.at(m[1]));
            else if(std::regex_match(terminator,m,branch)){b.condition=m[1];b.next={labels.at(m[2]),labels.at(m[3])};}
            else if(terminator!="ret void")return result;
            b.end=last;for(auto next:b.next)blocks[next].pred.push_back(i);
        }
        std::vector<std::set<unsigned>> dom(blocks.size());std::set<unsigned> all;for(unsigned i=0;i<blocks.size();++i)all.insert(i);
        dom[0]={0};for(unsigned i=1;i<blocks.size();++i)dom[i]=all;
        bool changed=true;unsigned iterations=0;while(changed){if(++iterations>4096)return result;changed=false;for(unsigned i=1;i<blocks.size();++i){std::set<unsigned> d=all;if(blocks[i].pred.empty())d.clear();for(auto pred:blocks[i].pred){std::set<unsigned> next;std::set_intersection(d.begin(),d.end(),dom[pred].begin(),dom[pred].end(),std::inserter(next,next.end()));d=std::move(next);}d.insert(i);if(d!=dom[i]){dom[i]=std::move(d);changed=true;}}}
        const std::string atom=R"((%[A-Za-z0-9_.$]+|[-+0-9A-Fa-f.xEe]+))";
        const std::regex product("^%[A-Za-z0-9_.$]+ = fmul fast float "+atom+", "+atom+"$");
        const std::regex addition("^%[A-Za-z0-9_.$]+ = f(?:add|sub) fast float "+atom+", "+atom+"$");
        const std::regex phi(R"(^%[A-Za-z0-9_.$]+ = phi float (.*)$)"),incoming("\\[ "+atom+", %[A-Za-z0-9_.$]+ \\]");
        auto proof=[&](const std::set<unsigned>& region,unsigned start,unsigned split,const std::string& stop){
            std::map<std::string,ZeroSet> cache;std::set<std::string> active;
            std::function<ZeroSet(const std::string&)> visit=[&](const std::string& value)->ZeroSet{
                if(auto n=number(value))return {*n==0,{}};
                if(auto it=cache.find(value);it!=cache.end())return it->second;
                ZeroSet out;out.values.insert(value);const auto d=definitions.find(value);
                if(value==stop||d==definitions.end()||!region.contains(d->second.block)||(d->second.block==start&&d->second.line<split)||!active.insert(value).second)return out;
                std::smatch match;const auto& text=code[d->second.line];
                if(std::regex_match(text,match,product))out=unite(out,unite(visit(match[1]),visit(match[2])));
                else if(std::regex_match(text,match,addition))out=unite(out,intersect(visit(match[1]),visit(match[2])));
                else if(std::regex_match(text,match,phi)){const auto list=match[1].str();ZeroSet common{true,{}};bool any=false;for(auto it=std::sregex_iterator(list.begin(),list.end(),incoming);it!=std::sregex_iterator();++it){common=intersect(common,visit((*it)[1]));any=true;}if(any)out=unite(out,common);}
                active.erase(value);cache[value]=out;return out;
            };
            // Evaluate eagerly; returning a closure here would dangle cache.
            std::map<std::string,ZeroSet> values;for(const auto& [name,d]:definitions)if(region.contains(d.block)&&(d.block!=start||d.line>=split))values[name]=visit(name);return values;
        };
        std::vector<Candidate> candidates;
        for(unsigned end=0;end<blocks.size();++end){
            if(blocks[end].next.size()!=1)continue;std::vector<std::string> escapes;
            for(const auto& [value,d]:definitions)if(d.block==end&&std::any_of(users[value].begin(),users[value].end(),[&](unsigned b){return b!=end;}))escapes.push_back(value);
            if(escapes.empty()||escapes.size()>12)continue;
            const auto local=proof({end},end,blocks[end].begin,{});ZeroSet common{true,{}};for(const auto& value:escapes)common=intersect(common,local.at(value));
            for(const auto& factor:common.values){const auto factor_def=definitions.find(factor);if(factor_def==definitions.end())continue;
                for(auto start:dom[end]){
                    Candidate c;c.start=start;c.end=end;c.factor=factor;c.split=blocks[start].begin;
                    if(factor_def->second.block==start)c.split=factor_def->second.line+1;
                    else if(!dom[start].contains(factor_def->second.block))continue;
                    while(c.split<blocks[start].end&&(code[c.split].empty()||code[c.split].find(" = phi ")!=std::string::npos))++c.split;
                    if(c.split>=blocks[start].end-1)continue;
                    bool valid=true;std::map<unsigned,unsigned> color;
                    std::function<void(unsigned)> walk=[&](unsigned b){if(!valid)return;if(color[b]==1){valid=false;return;}if(color[b]==2)return;color[b]=1;c.region.insert(b);if(c.region.size()>48){valid=false;return;}if(b!=end){if(blocks[b].next.empty()){valid=false;return;}for(auto next:blocks[b].next)walk(next);}color[b]=2;};walk(start);
                    if(!valid||!c.region.contains(end))continue;
                    for(auto b:c.region)if(b!=start)for(auto pred:blocks[b].pred)if(!c.region.contains(pred))valid=false;
                    if(!valid)continue;
                    for(auto b:c.region)for(unsigned i=b==start?c.split:blocks[b].begin;i<blocks[b].end-1;++i){if(code[i].starts_with("call void ")||code[i].find("store ")!=std::string::npos||code[i].find("atomic")!=std::string::npos)valid=false;}
                    if(!valid)continue;
                    const auto zeros=proof(c.region,start,c.split,factor);
                    for(const auto& [value,d]:definitions)if(c.region.contains(d.block)&&(d.block!=start||d.line>=c.split)&&
                        std::any_of(users[value].begin(),users[value].end(),[&](unsigned b){return !c.region.contains(b);})){const auto& z=zeros.at(value);if(!z.all&&!z.values.contains(factor)){valid=false;break;}c.outputs.push_back(value);}
                    if(!valid||c.outputs.empty()||c.outputs.size()>12)continue;
                    // Estimate only paths reachable when factor==0. This avoids
                    // claiming to skip work an existing zero check already skips.
                    std::map<std::string,std::optional<bool>> bool_cache;std::set<std::string> bool_active;
                    std::function<std::optional<bool>(const std::string&)> truth=[&](const std::string& v)->std::optional<bool>{
                        if(v=="true")return true;if(v=="false")return false;if(bool_cache.contains(v))return bool_cache[v];if(!bool_active.insert(v).second)return {};
                        std::optional<bool> r;const auto d=definitions.find(v);if(d!=definitions.end()){
                            std::smatch q;const std::regex cmp("^%[A-Za-z0-9_.$]+ = fcmp(?: fast)? (oeq|one|olt|ole|ogt|oge) float "+atom+", "+atom+"$");
                            const std::regex boolean(R"(^%[A-Za-z0-9_.$]+ = (and|or) i1 (%[A-Za-z0-9_.$]+|true|false), (%[A-Za-z0-9_.$]+|true|false)$)");
                            if(std::regex_match(code[d->second.line],q,cmp)){auto a=q[2].str()==factor?std::optional<float>(0.0f):number(q[2]);auto b=q[3].str()==factor?std::optional<float>(0.0f):number(q[3]);if(a&&b){const auto op=q[1].str();r=op=="oeq"?*a==*b:op=="one"?*a!=*b:op=="olt"?*a<*b:op=="ole"?*a<=*b:op=="ogt"?*a>*b:*a>=*b;}}
                            else if(std::regex_match(code[d->second.line],q,boolean)){const auto a=truth(q[2]),b=truth(q[3]);if(q[1]=="and"){if((a&&!*a)||(b&&!*b))r=false;else if(a&&b)r=*a&&*b;}else{if((a&&*a)||(b&&*b))r=true;else if(a&&b)r=*a||*b;}}
                        }
                        bool_active.erase(v);bool_cache[v]=r;return r;
                    };
                    std::set<unsigned> reachable;std::function<void(unsigned)> cost=[&](unsigned b){if(!reachable.insert(b).second)return;for(unsigned i=b==start?c.split:blocks[b].begin;i<blocks[b].end-1;++i)if(!code[i].empty()&&code[i].find(" = phi ")==std::string::npos)c.score+=code[i].find("@dx.op.sample")!=std::string::npos?8:1;
                        if(b==end)return;auto test=blocks[b].condition.empty()?std::optional<bool>{}:truth(blocks[b].condition);if(test)cost(blocks[b].next[*test?0:1]);else for(auto next:blocks[b].next)cost(next);};cost(start);
                    if(c.score>=32){
                        std::set<std::string> seen;
                        std::function<bool(const std::string&)> clamped=[&](const std::string& value){if(seen.size()>128||!seen.insert(value).second)return false;auto d=definitions.find(value);if(d==definitions.end())return false;const auto& text=code[d->second.line];
                            if(text.find("@dx.op.unary.f32(i32 7,")!=text.npos)return true;
                            if(text.find("@dx.op.binary.f32(i32 35,")!=text.npos&&text.find("float 0.000000e+00")!=text.npos)return true;
                            for(const auto& dependency:line_refs[d->second.line])if(clamped(dependency))return true;return false;};
                        c.zero_prone=clamped(c.factor);candidates.push_back(std::move(c));
                    }
                }
            }
        }
        std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.zero_prone!=b.zero_prone?a.zero_prone>b.zero_prone:a.score>b.score;});
        std::vector<Candidate> selected;std::set<unsigned> occupied;
        for(auto& c:candidates){if(selected.size()>=8)break;if(std::any_of(c.region.begin(),c.region.end(),[&](auto b){return occupied.contains(b);}))continue;occupied.insert(c.region.begin(),c.region.end());selected.push_back(std::move(c));}
        if(selected.empty())return result;
        std::map<unsigned,std::string> before,after;std::map<unsigned,std::map<std::string,std::string>> renames;
        std::map<std::string,std::string> predecessors;
        for(unsigned k=0;k<selected.size();++k){const auto& c=selected[k];const auto suffix=std::to_string(k),full="arc_zero_full"+suffix,merge="arc_zero_merge"+suffix;
            before[c.split]="  %arc_zero_test"+suffix+" = fcmp oeq float "+c.factor+", 0.000000e+00\n  %arc_zero_skip"+suffix+" = and i1 %arc_zero_enabled, %arc_zero_test"+suffix+
                "\n  br i1 %arc_zero_skip"+suffix+", label %"+merge+", label %"+full+"\n"+full+":\n";
            std::map<std::string,std::string> rename;unsigned j=0;for(const auto& value:c.outputs)rename[value]="%arc_zero_value"+suffix+"_"+std::to_string(j++);
            for(auto b:c.region)for(unsigned i=b==c.start?c.split:blocks[b].begin;i<blocks[b].end-1;++i)renames[i]=rename;
            const auto terminator=blocks[c.end].end-1;std::ostringstream tail;tail<<"  br label %"<<merge<<"\n"<<merge<<":\n";
            for(const auto& value:c.outputs)tail<<"  "<<value<<" = phi float [ 0.000000e+00, %"<<blocks[c.start].label<<" ], [ "<<rename[value]<<", %"<<(c.start==c.end?full:blocks[c.end].label)<<" ]\n";
            before[terminator]+=tail.str();predecessors[blocks[c.start].label]=full;predecessors[blocks[c.end].label]=merge;
            result.estimated_skipped_operations+=c.score;++result.regions;
        }
        std::ostringstream transformed;
        for(unsigned i=0;i<lines.size();++i){if(auto insert=before.find(i);insert!=before.end())transformed<<insert->second;auto text=lines[i];
            if(text.find(" = phi ")!=text.npos)for(const auto& [old_label,new_label]:predecessors){const std::regex incoming_ref(",\\s*%"+old_label+"\\s*\\]");text=std::regex_replace(text,incoming_ref,", %"+new_label+" ]");}
            if(auto names=renames.find(i);names!=renames.end()){std::string replaced;std::size_t last=0;for(auto it=std::sregex_iterator(text.begin(),text.end(),ref);it!=std::sregex_iterator();++it){const auto position=static_cast<std::size_t>(it->position());replaced+=text.substr(last,position-last);const auto name=names->second.find(it->str());replaced+=name==names->second.end()?it->str():name->second;last=position+it->length();}text=replaced+text.substr(last);}
            transformed<<text<<'\n';
            if(code[i].starts_with("%arc_coarse_control ="))transformed<<"  %arc_zero_controls = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %arc_coarse_control, i32 1)\n  %arc_zero_flag = extractvalue %dx.types.CBufRet.i32 %arc_zero_controls, 1\n  %arc_zero_enabled = icmp eq i32 %arc_zero_flag, 1\n";
        }
        result.ir=transformed.str();return result;
    }catch(const std::exception&){return ZeroFactorTransform{std::string(input)};}
}
}
