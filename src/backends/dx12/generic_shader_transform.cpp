#include "generic_shader_transform.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <functional>
#include <cmath>

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
bool group_barrier_proof(const std::vector<std::string>& lines,std::size_t begin,std::size_t end){
    std::map<std::string,std::set<std::string>> edges;std::set<std::string> exits,barriers;std::string block="entry",entry=block;std::smatch m;
    const std::regex numbered(R"(^; <label>:([0-9]+).*$)"),target(R"(label %([A-Za-z0-9_.$]+))");
    bool first=true;
    for(std::size_t i=begin+1;i<end;++i){auto c=code(lines[i]);if(std::regex_match(lines[i],m,numbered)){block=m[1];if(first)entry=block;first=false;continue;}
        if(c.ends_with(':')){block=c.substr(0,c.size()-1);if(first)entry=block;first=false;continue;}if(c.empty())continue;first=false;edges[block];
        if(c.starts_with("ret "))exits.insert(block);
        if(c.find("@dx.op.barrier(")!=c.npos){if(c!="call void @dx.op.barrier(i32 80, i32 9)")return false;barriers.insert(block);}
        if(c.starts_with("br ")||c.starts_with("switch "))for(auto it=std::sregex_iterator(c.begin(),c.end(),target);it!=std::sregex_iterator();++it)edges[block].insert((*it)[1]);
    }
    if(barriers.empty()||exits.empty())return false;
    auto reachable=[&](std::string start,const std::string& blocked){std::set<std::string> seen;std::vector<std::string> work{start};while(!work.empty()){auto at=work.back();work.pop_back();if(at==blocked||!seen.insert(at).second)continue;for(const auto& next:edges[at])work.push_back(next);}return seen;};
    for(const auto& barrier:barriers){const auto without=reachable(entry,barrier);for(const auto& exit:exits)if(without.contains(exit))return false;
        for(const auto& next:edges[barrier])if(next==barrier||reachable(next,"").contains(barrier))return false;}
    return true;
}
// Prove private/constant flat array accesses before admitting LLVM memory ops.
// Unknown indices, pointer escapes and writable global storage remain declined.
bool local_memory_proof(const std::vector<std::string>& lines,std::size_t begin,std::size_t end,const std::array<unsigned,3>& threads){
    struct Array {unsigned count;std::string type;bool readonly;bool shared{};};
    std::map<std::string,Array> arrays,pointers;std::map<std::string,std::uint64_t> upper;
    const std::string symbol=R"((%[A-Za-z0-9_.$]+|@"(?:[^"\\]|\\.)*"|@[A-Za-z0-9_.$]+))";
    const std::regex global("^"+symbol+R"( = .*constant \[([0-9]+) x (float|half|i32)\].*$)");
    const std::regex shared("^"+symbol+R"( = .*addrspace\(3\) global \[([0-9]+) x (float|half|i32)\].*$)");
    const std::regex local("^"+symbol+R"( = alloca \[([0-9]+) x (float|half|i32)\](?:, align [0-9]+)?$)");
    const std::regex gep("^"+symbol+R"( = getelementptr(?: inbounds)? \[([0-9]+) x (float|half|i32)\], \[[0-9]+ x (?:float|half|i32)\](?: addrspace\(3\))?\* )"+symbol+R"(, i32 0, i32 ([%A-Za-z0-9_.$]+)(?:, !.*)?$)");
    const std::regex binary(R"(^(%[A-Za-z0-9_.$]+) = (and|or|xor|urem|udiv|add|mul|shl|lshr)(?: nuw| nsw)* i32 ([%A-Za-z0-9_.$]+), ([%A-Za-z0-9_.$]+)$)");
    const std::regex flat(R"(^(%[A-Za-z0-9_.$]+) = call i32 @dx.op.flattenedThreadIdInGroup.i32\(i32 96\).*$)");
    const std::regex thread(R"(^(%[A-Za-z0-9_.$]+) = call i32 @dx.op.threadIdInGroup.i32\(i32 95, i32 ([012])\).*$)");
    auto bound=[&](const std::string& name)->std::uint64_t{if(!name.empty()&&std::isdigit(static_cast<unsigned char>(name[0])))return number(name);auto i=upper.find(name);return i==upper.end()?UINT64_MAX:i->second;};
    std::smatch m;
    for(std::size_t i=0;i<lines.size();++i){const auto c=code(lines[i]);if(i<begin&&std::regex_match(c,m,global))arrays.emplace(m[1].str(),Array{number(m[2].str()),m[3].str(),true});
        if(i<begin&&std::regex_match(c,m,shared)){const auto n=number(m[2].str());if(!n||n>8192)return false;arrays.emplace(m[1].str(),Array{n,m[3].str(),false,true});}
        if(i>begin&&i<end&&std::regex_match(c,m,local)){const auto n=number(m[2].str());if(!n||n>4096)return false;arrays.emplace(m[1].str(),Array{n,m[3].str(),false});}}
    std::map<std::string,std::string> definitions,owner,terminators;std::map<std::string,std::set<std::string>> predecessors;
    std::string block="0";const std::regex numbered(R"(^; <label>:([0-9]+).*$)"),edge(R"(label %([A-Za-z0-9_.$]+))");
    for(std::size_t i=begin+1;i<end;++i){const auto c=code(lines[i]);if(std::regex_match(lines[i],m,numbered)){block=m[1];continue;}if(c.ends_with(':')){block=c.substr(0,c.size()-1);continue;}if(c.empty())continue;
        terminators[block]=c;const auto eq=c.find(" = ");if(eq!=c.npos){const auto id=c.substr(0,eq);definitions[id]=c;owner[id]=block;}
        if(c.starts_with("br ")||c.starts_with("switch "))for(auto e=std::sregex_iterator(c.begin(),c.end(),edge);e!=std::sregex_iterator();++e)predecessors[(*e)[1]].insert(block);
    }
    const std::regex phi(R"(^(%[A-Za-z0-9_.$]+) = phi i32 \[ ([-%A-Za-z0-9_.$]+), %([A-Za-z0-9_.$]+) \], \[ ([-%A-Za-z0-9_.$]+), %([A-Za-z0-9_.$]+) \].*$)");
    const std::regex increment(R"(^(%[A-Za-z0-9_.$]+) = add(?: nuw| nsw)* i32 (%[A-Za-z0-9_.$]+), 1$)");
    const std::regex condition(R"(^(%[A-Za-z0-9_.$]+) = icmp (ult|slt|eq|ne) i32 (%[A-Za-z0-9_.$]+), ([0-9]+)$)");
    const std::regex branch(R"(^br i1 (%[A-Za-z0-9_.$]+), label %([A-Za-z0-9_.$]+), label %([A-Za-z0-9_.$]+)(?:, !.*)?$)");
    for(const auto& [id,definition]:definitions){if(!std::regex_match(definition,m,phi))continue;
        auto initial=m[2].str(),initial_block=m[3].str(),back=m[4].str(),latch=m[5].str();if(initial!="0"){std::swap(initial,back);std::swap(initial_block,latch);}if(initial!="0"||!definitions.contains(back))continue;
        const auto header=owner.at(id);if(predecessors[header]!=std::set<std::string>{initial_block,latch})continue;
        if(!std::regex_match(definitions.at(back),m,increment)||m[2]!=id||owner.at(back)!=latch)continue;
        if(!std::regex_match(terminators[latch],m,branch))continue;const auto test=m[1].str(),yes=m[2].str(),no=m[3].str();
        if(!definitions.contains(test)||owner.at(test)!=latch||!std::regex_match(definitions.at(test),m,condition)||m[3]!=back)continue;
        const auto comparison=m[2].str();const auto count=number(m[4].str());if(!count||count>4096)continue;
        if((comparison=="eq"&&no==header&&yes!=header)||(comparison!="eq"&&yes==header&&no!=header))upper[id]=count-1;
    }
    for(unsigned iteration=0;iteration<32;++iteration){bool changed=false;
        for(std::size_t i=begin+1;i<end;++i){const auto c=code(lines[i]);std::uint64_t value=UINT64_MAX;std::string name;
            if(std::regex_match(c,m,thread)){name=m[1];value=threads[number(m[2].str())]-1;}
            else if(std::regex_match(c,m,flat)){name=m[1];value=threads[0]*threads[1]*threads[2]-1;}
            else if(std::regex_match(c,m,binary)){name=m[1];const auto op=m[2].str();const auto a=bound(m[3]),b=bound(m[4]);
                if(op=="and")value=std::min(a,b);
                else if(op=="urem"&&b&&b<UINT32_MAX&&!m[4].str().starts_with('%'))value=b-1;
                else if(op=="add"&&a<=UINT32_MAX&&b<=UINT32_MAX&&a+b<=UINT32_MAX)value=a+b;
                else if(op=="mul"&&a<=UINT32_MAX&&b<=UINT32_MAX&&a*b<=UINT32_MAX)value=a*b;
                else if(op=="shl"&&a<=UINT32_MAX&&b<32&&(a<<b)<=UINT32_MAX)value=a<<b;
                else if(op=="lshr"&&a<=UINT32_MAX&&b<32&&!m[4].str().starts_with('%'))value=a>>b;
                else if(op=="udiv"&&a<=UINT32_MAX&&b&&b<=UINT32_MAX&&!m[4].str().starts_with('%'))value=a/b;
                else if((op=="or"||op=="xor")&&a<=UINT32_MAX&&b<=UINT32_MAX){value=1;while(value<=std::max(a,b)&&value<(1ull<<32))value<<=1;--value;}
            }
            if(value!=UINT64_MAX&&(!upper.contains(name)||upper[name]!=value)){upper[name]=value;changed=true;}
        }if(!changed)break;
    }
    // Small signed induction ranges and exact float coordinate arithmetic.
    // Only finite bounds are consumed by GEP proofs; unknowns remain rejected.
    using Interval=std::pair<double,double>;std::map<std::string,Interval> induction;
    for(const auto& [id,definition]:definitions)if(std::regex_match(definition,m,phi)){
        auto initial=m[2].str(),initial_block=m[3].str(),back=m[4].str(),latch=m[5].str();if(initial.starts_with('%')){std::swap(initial,back);std::swap(initial_block,latch);}
        if(initial.empty()||initial.starts_with('%')||!definitions.contains(back))continue;const auto start=std::stoll(initial);if(start< -4096||start>4096)continue;const auto header=owner.at(id);if(predecessors[header]!=std::set<std::string>{initial_block,latch})continue;
        if(!std::regex_match(definitions.at(back),m,increment)||m[2]!=id||owner.at(back)!=latch)continue;
        if(!std::regex_match(terminators[latch],m,branch))continue;const auto test=m[1].str(),yes=m[2].str(),no=m[3].str();if(!definitions.contains(test)||!std::regex_match(definitions.at(test),m,condition)||m[3]!=back)continue;
        const auto comparison=m[2].str();const auto count=number(m[4].str());if(count>4096||start>=count)continue;
        if((comparison=="slt"&&yes==header&&no!=header)||(comparison=="eq"&&no==header&&yes!=header))induction[id]={double(start),double(count-1)};
    }
    const Interval unknown{-1.e100,1.e100};
    const std::regex cast(R"(^(%[A-Za-z0-9_.$]+) = (sitofp|uitofp|fptosi|fptoui) (?:i32|float) (%[A-Za-z0-9_.$]+) to (?:float|i32)$)");
    const std::regex floatadd(R"(^(%[A-Za-z0-9_.$]+) = fadd(?: fast)? float ([^,]+), (.+)$)");
    std::function<Interval(const std::string&,unsigned)> interval;
    interval=[&](const std::string& name,unsigned depth)->Interval{
        if(depth>32)return unknown;if(induction.contains(name))return induction[name];if(!name.starts_with('%')){try{std::size_t used{};const auto value=std::stod(name,&used);if(used==name.size()&&std::isfinite(value)&&std::abs(value)<=1048576)return {value,value};}catch(...){}return unknown;}
        if(upper.contains(name)&&upper[name]<=1048576)return {0,double(upper[name])};if(!definitions.contains(name))return unknown;std::smatch m;
        if(std::regex_match(definitions.at(name),m,cast)){auto range=interval(m[3],depth+1);if(range.first< -1048576||range.second>1048576)return unknown;
            if(m[2]=="fptosi"||m[2]=="fptoui"){if(m[2]=="fptoui"&&range.first<0)return unknown;return {std::trunc(range.first),std::trunc(range.second)};}return {double(float(range.first)),double(float(range.second))};}
        if(std::regex_match(definitions.at(name),m,floatadd)){const auto a=interval(m[2],depth+1),b=interval(m[3],depth+1);if(a.first+b.first< -1048576||a.second+b.second>1048576)return unknown;return {double(float(a.first)+float(b.first)),double(float(a.second)+float(b.second))};}
        return unknown;
    };
    std::set<std::string> blocks;for(const auto& [b,t]:terminators)blocks.insert(b);
    std::map<std::string,std::set<std::string>> dom;for(const auto& b:blocks)dom[b]=predecessors[b].empty()?std::set<std::string>{b}:blocks;
    for(unsigned round=0;round<blocks.size()+1;++round){bool changed=false;for(const auto& b:blocks)if(!predecessors[b].empty()){auto common=blocks;for(const auto& pred:predecessors[b]){std::set<std::string> next;std::set_intersection(common.begin(),common.end(),dom[pred].begin(),dom[pred].end(),std::inserter(next,next.end()));common=std::move(next);}common.insert(b);if(common!=dom[b]){dom[b]=std::move(common);changed=true;}}if(!changed)break;}
    std::map<std::string,std::map<std::string,std::uint64_t>> constraints;
    for(const auto& [b,term]:terminators)if(std::regex_match(term,m,branch)){const auto test=m[1].str(),yes=m[2].str();if(predecessors[yes]!=std::set<std::string>{b}||!definitions.contains(test))continue;
        if(std::regex_match(definitions.at(test),m,condition)&&m[2]=="ult"){const auto count=number(m[4].str());if(count)for(const auto& target:blocks)if(dom[target].contains(yes))constraints[target][m[3].str()]=count-1;}}
    std::function<std::uint64_t(const std::string&,const std::string&,unsigned)> path_bound;
    path_bound=[&](const std::string& name,const std::string& at,unsigned depth)->std::uint64_t{auto result=bound(name);const auto numeric=interval(name,0);if(numeric.first>=0&&numeric.second<=UINT32_MAX)result=std::min(result,std::uint64_t(numeric.second));if(const auto found=constraints[at].find(name);found!=constraints[at].end())result=std::min(result,found->second);if(depth>64||!definitions.contains(name))return result;
        std::smatch match;if(!std::regex_match(definitions.at(name),match,binary))return result;const auto op=match[2].str(),left=match[3].str(),right=match[4].str();const auto a=path_bound(left,at,depth+1),b=path_bound(right,at,depth+1);std::uint64_t value=UINT64_MAX;
        if(op=="and")value=std::min(a,b);else if(op=="urem"&&b&&b<=UINT32_MAX&&!right.starts_with('%'))value=b-1;else if(op=="udiv"&&a<=UINT32_MAX&&b&&b<=UINT32_MAX&&!right.starts_with('%'))value=a/b;
        else if(op=="add"&&a<=UINT32_MAX&&b<=UINT32_MAX&&a+b<=UINT32_MAX)value=a+b;else if(op=="mul"&&a<=UINT32_MAX&&b<=UINT32_MAX&&a*b<=UINT32_MAX)value=a*b;
        else if(op=="shl"&&a<=UINT32_MAX&&b<32&&(a<<b)<=UINT32_MAX)value=a<<b;else if(op=="lshr"&&a<=UINT32_MAX&&b<32&&!right.starts_with('%'))value=a>>b;
        else if((op=="or"||op=="xor")&&a<=UINT32_MAX&&b<=UINT32_MAX){value=1;while(value<=std::max(a,b)&&value<(1ull<<32))value<<=1;--value;}
        return std::min(result,value);
    };
    for(std::size_t i=begin+1;i<end;++i){const auto c=code(lines[i]);
        if(c.find(" = alloca ")!=c.npos){if(!std::regex_match(c,m,local))return false;continue;}
        if(c.find(" = getelementptr ")!=c.npos){if(!std::regex_match(c,m,gep))return false;const auto a=arrays.find(m[4]);if(a==arrays.end()||a->second.count!=number(m[2].str())||a->second.type!=m[3].str()||path_bound(m[5],owner[m[1].str()],0)>=a->second.count)return false;pointers.emplace(m[1],a->second);continue;}
        if(c.find(" = load ")!=c.npos||c.starts_with("store ")){
            const bool store=c.starts_with("store ");const auto at=store?6:c.find(" = load ")+8;const auto args=fields(c.substr(at));if(args.size()<2)return false;
            const auto space=args[1].find_last_of(' ');if(space==args[1].npos)return false;const auto pointer=pointers.find(args[1].substr(space+1));
            if(pointer==pointers.end()||(store&&pointer->second.readonly)||args[1].substr(0,space)!=pointer->second.type+(pointer->second.shared?" addrspace(3)*":"*"))return false;
            if((store&&!args[0].starts_with(pointer->second.type+" "))||(!store&&args[0]!=pointer->second.type))return false;
        }
    }
    return true;
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

Transform coarse_compute(std::string_view input,unsigned x_rate,unsigned y_rate,bool runtime_control,unsigned requested_control_space,bool execution_marker) {
    Transform out;
    auto reject=[&](std::string reason){out.reason=std::move(reason);out.ir.clear();return out;};
    if(execution_marker&&!runtime_control)return reject("execution_marker_requires_runtime_control");
    try {
        if(input.size()>8*1024*1024||input.empty())return reject("ir_size");
        if((x_rate!=1&&x_rate!=2&&x_rate!=4)||(y_rate!=1&&y_rate!=2&&y_rate!=4))return reject("rate");
        std::string text(input);while(!text.empty()&&text.back()=='\0')text.pop_back();
        if(text.find("arc_coarse_")!=text.npos)return reject("already_transformed");
        std::map<unsigned,std::vector<std::string>> metadata;
        std::map<std::string,unsigned> named;
        std::vector<std::string> lines;std::istringstream stream(text);std::string line;std::smatch m;
        const std::regex md(R"(^!([0-9]+) = !\{(.*)\}$)"), nm(R"(^!(dx\.[A-Za-z]+) = !\{!([0-9]+)\}$)");
        std::string pending_switch;
        unsigned functions=0,next_metadata=0;std::size_t begin=0,end=0;
        const std::regex metadata_id(R"(^!([0-9]+) = )");
        while(std::getline(stream,line)){
            line=trim(line);
            if(!pending_switch.empty()){pending_switch+=' '+code(line);if(line.find(']')==line.npos)continue;line=std::move(pending_switch);pending_switch.clear();}
            else if(code(line).starts_with("switch ")&&line.find(']')==line.npos){pending_switch=code(line);continue;}
            std::smatch identity;if(std::regex_search(line,identity,metadata_id)){const auto id=number(identity[1].str());if(id>=1000000)return reject("metadata_capacity");next_metadata=std::max(next_metadata,id+1);}
            if(std::regex_match(line,m,md))metadata.emplace(number(m[1].str()),fields(m[2].str()));
            else if(std::regex_match(line,m,nm))named.emplace(m[1].str(),number(m[2].str()));
            if(line.starts_with("define ")){++functions;begin=lines.size();}
            if(line=="}")end=lines.size();
            lines.push_back(line);
        }
        if(!pending_switch.empty())return reject("switch_shape");
        if(functions!=1||end<=begin||!std::regex_match(lines[begin],std::regex(R"(^define void @[A-Za-z_.$][A-Za-z0-9_.$]*\(\) \{$)")))return reject("entry_shape");
        const auto& model=metadata.at(named.at("dx.shaderModel"));
        if(model.size()!=3||model[0]!="!\"cs\""||integer(model[1])!=6||integer(model[2])>6)return reject("shader_model");
        const bool binding_handles=integer(model[2])==6;
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
        out.group_shared=text.find("addrspace(3)")!=text.npos||text.find("@dx.op.barrier(")!=text.npos;
        if(out.group_shared&&(!runtime_control||!group_barrier_proof(lines,begin,end)))return reject("group_barrier_convergence_unproven");
        if(!local_memory_proof(lines,begin,end,out.threads))return reject("unproven_local_memory");
        std::set<unsigned> unsupported_uavs;
        const auto& lists=metadata.at(named.at("dx.resources"));if(lists.size()!=4)return reject("resource_lists");
        for(unsigned cls=0;cls<4;++cls){if(lists[cls]=="null")continue;
            for(const auto& resource:metadata.at(metadata_ref(lists[cls]))){
                const auto& r=metadata.at(metadata_ref(resource));if(r.size()<7)return reject("resource_metadata");
                ResourceContract c{cls,integer(r[0]),integer(r[4]),integer(r[3]),integer(r[5]),integer(r[6])};
                if(!c.count||(c.count!=UINT32_MAX&&std::uint64_t(c.shader_register)+c.count>UINT32_MAX))return reject("resource_range");
                // A runtime submission must resolve a finite set of uniform
                // indices before any unbounded table can be activated.
                if(c.count==UINT32_MAX&&(!runtime_control||cls==2))return reject("unbounded_resource_requires_runtime_proof");
                if(cls==1&&(c.kind!=2||(!runtime_control&&c.count!=1)||r.size()!=11||r[7]!="i1 false"||r[8]!="i1 false"||r[9]!="i1 false"))unsupported_uavs.insert(c.range_id);
                if(std::any_of(out.resources.begin(),out.resources.end(),[&](const auto& old){return old.resource_class==cls&&old.range_id==c.range_id;}))return reject("duplicate_range");
                out.resources.push_back(c);
            }
        }
        struct Handle {unsigned cls{},range{},properties0{},properties1{};bool annotated{};};std::map<std::string,Handle> handles;
        std::set<std::string> ray_queries;
        std::array<std::string,3> ids;std::array<std::set<std::string>,3> thread_ids;
        std::string entry_label="arc_coarse_orig_0";bool named_entry=false;
        for(std::size_t i=begin+1;i<end;++i){const auto first=code(lines[i]);if(first.empty())continue;if(first.ends_with(':')){entry_label=first.substr(0,first.size()-1);named_entry=true;}break;}
        struct Store {std::size_t line{};std::vector<std::string> args;};std::vector<Store> stores;
        const std::set<std::string> allowed_calls={"dx.op.createHandle","dx.op.createHandleFromBinding","dx.op.annotateHandle","dx.op.threadId.i32","dx.op.textureLoad.f32","dx.op.textureLoad.i32",
            "dx.op.textureStore.f32","dx.op.cbufferLoadLegacy.f32","dx.op.cbufferLoadLegacy.i32","dx.op.sampleLevel.f32","dx.op.sampleCmpLevelZero.f32",
            "dx.op.unary.f32","dx.op.binary.f32","dx.op.tertiary.f32","dx.op.unary.i32","dx.op.binary.i32","dx.op.tertiary.i32",
            "dx.op.dot2.f32","dx.op.dot3.f32","dx.op.dot4.f32","dx.op.bitcastI32toF32","dx.op.bitcastF32toI32",
            "dx.op.legacyF16ToF32","dx.op.legacyF32ToF16","dx.op.getDimensions",
            "dx.op.flattenedThreadIdInGroup.i32","dx.op.binaryWithTwoOuts.i32","dx.op.groupId.i32","dx.op.threadIdInGroup.i32",
            "dx.op.isSpecialFloat.f32","dx.op.isSpecialFloat.f16","dx.op.dot2.f16","dx.op.dot3.f16","dx.op.dot4.f16","dx.op.unary.f16","dx.op.binary.f16","dx.op.tertiary.f16",
            "dx.op.bufferLoad.f32","dx.op.bufferLoad.i32","dx.op.rawBufferLoad.f32","dx.op.rawBufferLoad.i32",
            "dx.op.barrier","dx.op.allocateRayQuery","dx.op.rayQuery_TraceRayInline","dx.op.rayQuery_Proceed.i1","dx.op.rayQuery_StateScalar.i32"};
        const std::set<std::string> allowed_instructions={"call","ret","br","phi","switch","alloca","getelementptr","load","store","add","sub","mul","udiv","sdiv","urem","srem","fadd","fsub","fmul","fdiv","frem",
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
                if(binding_handles)return reject("legacy_handle_in_sm66");
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
            } else if(name=="dx.op.createHandleFromBinding"){
                if(!binding_handles||value.empty()||args.size()!=4||args[0]!="i32 217"||args[3]!="i1 false"||
                    !args[1].starts_with("%dx.types.ResBind ")||!args[2].starts_with("i32 "))return reject("binding_handle_shape");
                const auto binding=args[1]=="%dx.types.ResBind zeroinitializer"?
                    std::vector<std::string>{"i32 0","i32 0","i32 0","i8 0"}:
                    (args[1].starts_with("%dx.types.ResBind {")&&args[1].ends_with('}')?
                        fields(args[1].substr(sizeof("%dx.types.ResBind {")-1,args[1].size()-sizeof("%dx.types.ResBind {")-1)):
                        std::vector<std::string>{});
                if(binding.size()!=4||!binding[3].starts_with("i8 "))return reject("binding_handle_shape");
                const unsigned lower=integer(binding[0]),upper=integer(binding[1]),space=integer(binding[2]),cls=number(std::string_view(binding[3]).substr(3));
                if(cls>3||upper<lower||args[2].find('%')!=std::string::npos)return reject("binding_handle_dynamic_index");
                const unsigned index=integer(args[2]);if(index<lower||index>upper)return reject("binding_handle_index");
                const auto resource=std::find_if(out.resources.begin(),out.resources.end(),[&](const auto& r){
                    return r.resource_class==cls&&r.shader_register==lower&&r.space==space&&r.count!=UINT32_MAX&&
                        std::uint64_t(lower)+r.count-1==upper;});
                if(resource==out.resources.end()||!handles.emplace(value,Handle{cls,resource->range_id}).second)return reject("binding_handle_range");
            } else if(name=="dx.op.annotateHandle"){
                if(!binding_handles||value.empty()||args.size()!=3||args[0]!="i32 216"||
                    !args[1].starts_with("%dx.types.Handle %")||!args[2].starts_with("%dx.types.ResourceProperties {")||!args[2].ends_with('}'))return reject("annotate_handle_shape");
                const auto source=args[1].substr(sizeof("%dx.types.Handle ")-1);
                const auto properties=fields(args[2].substr(sizeof("%dx.types.ResourceProperties {")-1,args[2].size()-sizeof("%dx.types.ResourceProperties {")-1));
                if(properties.size()!=2||!handles.contains(source)||handles.at(source).annotated)return reject("annotate_handle_source");
                auto handle=handles.at(source);handle.properties0=integer(properties[0]);handle.properties1=integer(properties[1]);handle.annotated=true;
                const auto resource=std::find_if(out.resources.begin(),out.resources.end(),[&](const auto& r){return r.resource_class==handle.cls&&r.range_id==handle.range;});
                if(resource==out.resources.end())return reject("annotate_handle_properties");
                if(handle.cls==2){if(handle.properties0!=13||handle.properties1!=resource->kind)return reject("annotate_handle_properties");}
                else if(handle.cls==3){if(handle.properties0!=14)return reject("annotate_handle_properties");}
                else if((handle.properties0&0xff)!=resource->kind||((handle.properties0>>12)&1)!=(handle.cls==1))return reject("annotate_handle_properties");
                if(!handles.emplace(value,handle).second)return reject("annotate_handle_duplicate");
            } else if(name=="dx.op.allocateRayQuery"){
                if(args.size()!=2||args[0]!="i32 178"||value.empty()||ray_queries.size()>=16)return reject("ray_query_allocation");
                ray_queries.insert(value);
            } else if(name.starts_with("dx.op.rayQuery_")){
                if(args.size()<2||!args[1].starts_with("i32 ")||!ray_queries.contains(args[1].substr(4)))return reject("nonlocal_ray_query");
                if(name=="dx.op.rayQuery_TraceRayInline"){
                    if(args.size()!=13||args[0]!="i32 179")return reject("ray_trace_shape");
                    const auto h=args[2].substr(args[2].find_last_of(' ')+1);
                    if(!handles.contains(h)||handles.at(h).cls!=0||(binding_handles&&!handles.at(h).annotated))return reject("ray_trace_handle");
                    const auto range=handles.at(h).range;
                    if(std::none_of(out.resources.begin(),out.resources.end(),[&](const auto& r){return r.resource_class==0&&r.range_id==range&&r.kind==16;}))return reject("ray_trace_resource");
                }else if((name=="dx.op.rayQuery_Proceed.i1"&&(args.size()!=2||args[0]!="i32 180"))||
                         (name=="dx.op.rayQuery_StateScalar.i32"&&(args.size()!=2||args[0]!="i32 184")))return reject("ray_query_operation");
            } else if(name=="dx.op.threadId.i32"){
                if(args.size()!=2)return reject("thread_id");const auto dim=integer(args[1]);if(dim>1)return reject("thread_id");if(ids[dim].empty())ids[dim]=value;thread_ids[dim].insert(value);
            } else if(name=="dx.op.textureStore.f32"){
                if(args.size()!=10||args[4]!="i32 undef"||(args[9]!="i8 15"&&args[9]!="i8 7"&&args[9]!="i8 3"&&args[9]!="i8 1"))return reject("store_shape");
                const auto h=args[1].substr(args[1].find_last_of(' ')+1);
                if(!handles.contains(h)||handles.at(h).cls!=1||(binding_handles&&!handles.at(h).annotated))return reject("store_handle");
                if(unsupported_uavs.contains(handles.at(h).range))return reject("uav_contract");
                stores.push_back({i,args});
            } else if(name.starts_with("dx.op.textureLoad")||name.starts_with("dx.op.sample")||name.starts_with("dx.op.bufferLoad")||name.starts_with("dx.op.rawBufferLoad")){
                if(args.size()<2)return reject("read_shape");const auto h=args[1].substr(args[1].find_last_of(' ')+1);
                if(!handles.contains(h)||handles.at(h).cls!=0||(binding_handles&&!handles.at(h).annotated))return reject("uav_or_unknown_read");
                if(name.starts_with("dx.op.sample")){if(args.size()<3)return reject("sample_shape");const auto sampler=args[2].substr(args[2].find_last_of(' ')+1);
                    if(!handles.contains(sampler)||handles.at(sampler).cls!=3||(binding_handles&&!handles.at(sampler).annotated))return reject("sample_sampler");}
            } else if(name.starts_with("dx.op.cbufferLoadLegacy")){
                if(args.size()<2)return reject("cbuffer_shape");const auto h=args[1].substr(args[1].find_last_of(' ')+1);
                if(!handles.contains(h)||handles.at(h).cls!=2||(binding_handles&&!handles.at(h).annotated))return reject("cbuffer_handle");
            } else if(name=="dx.op.getDimensions"){
                if(args.size()!=3)return reject("dimension_shape");const auto h=args[1].substr(args[1].find_last_of(' ')+1);
                if(!handles.contains(h)||handles.at(h).cls>1||(binding_handles&&!handles.at(h).annotated))return reject("dimension_handle");
            }
        }
        using Affine=std::array<std::int64_t,5>;std::map<std::string,Affine> coordinates;
        const std::regex coord_call(R"(^(%[A-Za-z0-9_.$]+) = call i32 @dx.op.(groupId|threadIdInGroup|threadId).i32\(i32 [0-9]+, i32 ([01])\).*$)");
        const std::regex coord_op(R"(^(%[A-Za-z0-9_.$]+) = (add|mul|shl)(?: nuw| nsw)* i32 ([%A-Za-z0-9_.$]+), ([%A-Za-z0-9_.$]+)$)");
        for(std::size_t i=begin+1;i<end;++i){const auto c=code(lines[i]);Affine value{};std::string name;bool known=false;
            if(std::regex_match(c,m,coord_call)){name=m[1];const auto axis=number(m[3].str());const auto kind=m[2].str();if(kind=="groupId")value[axis]=1;else if(kind=="threadIdInGroup")value[axis+2]=1;else{value[axis]=out.threads[axis];value[axis+2]=1;}known=true;}
            else if(std::regex_match(c,m,coord_op)){name=m[1];const auto a=m[3].str(),b=m[4].str(),op=m[2].str();
                if(op=="add"&&coordinates.contains(a)&&coordinates.contains(b)){for(unsigned k=0;k<5;++k)value[k]=coordinates[a][k]+coordinates[b][k];known=true;}
                else if(coordinates.contains(a)&&!b.starts_with('%')){value=coordinates[a];const auto n=number(b);if(op=="add"){value[4]+=n;known=true;}else if(op=="mul"||(op=="shl"&&n<16)){const auto factor=op=="shl"?1u<<n:n;if(factor<=1024){for(auto& v:value)v*=factor;known=true;}}}
            }
            if(!known||std::any_of(value.begin(),value.end(),[](auto v){return v<0||v>UINT32_MAX;}))continue;coordinates[name]=value;for(unsigned axis=0;axis<2;++axis){Affine expected{};expected[axis]=out.threads[axis];expected[axis+2]=1;if(value==expected){thread_ids[axis].insert(name);if(ids[axis].empty())ids[axis]=name;}}
        }
        std::erase_if(out.resources,[&](const auto& r){return std::none_of(handles.begin(),handles.end(),[&](const auto& h){return h.second.cls==r.resource_class&&h.second.range==r.range_id;});});
        if(ids[0].empty()||ids[1].empty()||stores.empty())return reject("no_pixel_outputs");
        for(const auto& s:stores)if(!s.args[2].starts_with("i32 ")||!s.args[3].starts_with("i32 ")||!thread_ids[0].contains(s.args[2].substr(4))||!thread_ids[1].contains(s.args[3].substr(4)))return reject("nonlocal_store");
        out.stores=static_cast<unsigned>(stores.size());
        unsigned control_range{},marker_range{};
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
            if(execution_marker){
                for(const auto& r:out.resources)if(r.resource_class==1)marker_range=std::max(marker_range,r.range_id+1);
                auto uavs=resource_lists[1]=="null"?std::vector<std::string>{}:metadata.at(metadata_ref(resource_lists[1]));
                const auto marker_id=next++,uav_list=next++;uavs.push_back("!"+std::to_string(marker_id));resource_lists[1]="!"+std::to_string(uav_list);
                appended_metadata="\n"+format(uav_list,uavs)+"\n!"+std::to_string(marker_id)+" = !{i32 "+std::to_string(marker_range)+", %arc_execution_buffer* undef, !\"\", i32 "+std::to_string(space)+", i32 0, i32 1, i32 11, i1 false, i1 false, i1 false, null}\n";
                auto flags=properties;bool found=false;
                for(std::size_t n=0;n<flags.size();n+=2)if(integer(flags[n])==0){if(!flags[n+1].starts_with("i64 "))return reject("shader_flags");flags[n+1]="i64 "+std::to_string(std::stoull(flags[n+1].substr(4))|16ull);found=true;}
                if(!found){flags.push_back("i32 0");flags.push_back("i64 16");}
                const auto property_id=metadata_ref(entries[4]);for(auto& l:lines)if(l.starts_with("!"+std::to_string(property_id)+" = "))l=format(property_id,flags);
                out.execution_marker=true;out.execution_marker_range=marker_range;
            }
            for(auto& l:lines)if(l.starts_with("!"+std::to_string(root_id)+" = "))l=format(root_id,resource_lists);
            appended_metadata+="\n"+format(list_id,cbuffers)+"\n!"+std::to_string(control_id)+" = !{i32 "+std::to_string(control_range)+
                ", %arc_coarse_control_buffer* undef, !\"\", i32 "+std::to_string(space)+", i32 0, i32 1, i32 "+(execution_marker?"144":"48")+", null}\n";
        }
        // Numeric SSA ids and anonymous block ids must be named before adding
        // instructions. Renumbering only definitions would corrupt PHI edges.
        const std::regex local(R"(%([0-9]+)\b)"), label(R"(^; <label>:([0-9]+).*$)");
        for(std::size_t i=begin+1;i<end;++i){
            if(std::regex_match(lines[i],m,label))lines[i]="arc_coarse_orig_"+m[1].str()+":";
            else lines[i]=std::regex_replace(lines[i],local,"%arc_coarse_orig_$1");
        }
        std::ostringstream generated;
        auto emit_handle=[&](std::string_view name,unsigned cls,unsigned reg,unsigned resource_space,unsigned range,unsigned upper,unsigned property0,unsigned property1){
            if(!binding_handles){generated<<"  %"<<name<<" = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 "<<cls<<", i32 "<<range<<", i32 "<<reg<<", i1 false)\n";return;}
            generated<<"  %"<<name<<"_binding = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 "<<reg<<", i32 "<<upper<<", i32 "<<resource_space<<", i8 "<<cls<<" }, i32 "<<reg<<", i1 false)\n"
                <<"  %"<<name<<" = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %"<<name<<"_binding, %dx.types.ResourceProperties { i32 "<<property0<<", i32 "<<property1<<" })\n";
        };
        if(text.find("declare i32 @dx.op.threadId.i32(")==text.npos)generated<<"declare i32 @dx.op.threadId.i32(i32, i32)\n";
        const bool needs_bounds=runtime_control||x_rate>1||y_rate>1;
        if(needs_bounds&&!runtime_control){
            if(text.find("%dx.types.Dimensions = type")==text.npos)generated<<"%dx.types.Dimensions = type { i32, i32, i32, i32 }\n";
            if(text.find("declare %dx.types.Dimensions @dx.op.getDimensions(")==text.npos)generated<<"declare %dx.types.Dimensions @dx.op.getDimensions(i32, %dx.types.Handle, i32)\n";
        }
        if(runtime_control){
            generated<<"%arc_coarse_control_buffer = type { ["<<(execution_marker?36:12)<<" x i32] }\n";
            if(execution_marker){generated<<"%arc_execution_buffer = type { i32 }\n";
                if(text.find("declare void @dx.op.bufferStore.i32(")==text.npos)generated<<"declare void @dx.op.bufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8)\n";}
            if(text.find("%dx.types.CBufRet.i32 = type")==text.npos)generated<<"%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }\n";
            if(text.find("declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(")==text.npos)
                generated<<"declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32)\n";
        }
        for(std::size_t i=0;i<lines.size();++i){
            generated<<lines[i]<<'\n';
            if(i==begin){
                if(execution_marker){
                    generated<<"arc_proof_entry:\n";
                    emit_handle("arc_proof_control",2,0,out.control_space,control_range,0,13,144);
                    generated<<"  %arc_proof_data = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %arc_proof_control, i32 4)\n";
                    for(unsigned j=0;j<4;++j)generated<<"  %arc_proof_value"<<j<<" = extractvalue %dx.types.CBufRet.i32 %arc_proof_data, "<<j<<"\n";
                    generated<<"  %arc_proof_epoch = or i32 %arc_proof_value0, %arc_proof_value1\n"
                        <<"  %arc_proof_enabled = icmp ne i32 %arc_proof_epoch, 0\n"
                        <<"  %arc_proof_x = call i32 @dx.op.threadId.i32(i32 93, i32 0)\n"
                        <<"  %arc_proof_y = call i32 @dx.op.threadId.i32(i32 93, i32 1)\n"
                        <<"  %arc_proof_xy = or i32 %arc_proof_x, %arc_proof_y\n"
                        <<"  %arc_proof_first = icmp eq i32 %arc_proof_xy, 0\n"
                        <<"  %arc_proof_write = and i1 %arc_proof_enabled, %arc_proof_first\n"
                        <<"  br i1 %arc_proof_write, label %arc_proof_store, label %arc_coarse_entry\narc_proof_store:\n";
                    emit_handle("arc_proof_buffer",1,0,out.control_space,marker_range,0,4107,0);
                    generated<<"  call void @dx.op.bufferStore.i32(i32 69, %dx.types.Handle %arc_proof_buffer, i32 0, i32 undef, i32 %arc_proof_value0, i32 %arc_proof_value1, i32 %arc_proof_value2, i32 %arc_proof_value3, i8 15)\n"
                        <<"  br label %arc_coarse_entry\n";
                }
                generated<<"arc_coarse_entry:\n";
                if(runtime_control){
                    emit_handle("arc_coarse_control",2,0,out.control_space,control_range,0,13,execution_marker?144:48);
                    generated<<"  %arc_coarse_values = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %arc_coarse_control, i32 0)\n"
                        <<"  %arc_coarse_width = extractvalue %dx.types.CBufRet.i32 %arc_coarse_values, 2\n"
                        <<"  %arc_coarse_height = extractvalue %dx.types.CBufRet.i32 %arc_coarse_values, 3\n";
                    for(unsigned d=0;d<2;++d){const char a=d?'y':'x';generated<<"  %arc_coarse_requested_"<<a<<" = extractvalue %dx.types.CBufRet.i32 %arc_coarse_values, "<<d<<"\n"
                        <<"  %arc_coarse_twice_"<<a<<" = icmp eq i32 %arc_coarse_requested_"<<a<<", 2\n"
                        <<"  %arc_coarse_four_"<<a<<" = icmp eq i32 %arc_coarse_requested_"<<a<<", 4\n"
                        <<"  %arc_coarse_enabled_"<<a<<" = or i1 %arc_coarse_twice_"<<a<<", %arc_coarse_four_"<<a<<"\n"
                        <<"  %arc_coarse_requested_safe_"<<a<<" = select i1 %arc_coarse_four_"<<a<<", i32 4, i32 2\n"
                        <<"  %arc_coarse_rate_"<<a<<" = select i1 %arc_coarse_enabled_"<<a<<", i32 %arc_coarse_requested_safe_"<<a<<", i32 1\n";}
                }else if(needs_bounds){
                    const auto& first=stores.front().args[1];const auto handle=handles.at(first.substr(first.find_last_of(' ')+1));
                    const auto resource=std::find_if(out.resources.begin(),out.resources.end(),[&](const auto& r){return r.resource_class==1&&r.range_id==handle.range;});
                    if(resource==out.resources.end())return reject("output_extent_handle");
                    emit_handle("arc_coarse_extent_handle",1,resource->shader_register,resource->space,handle.range,
                        resource->shader_register+resource->count-1,handle.properties0,handle.properties1);
                    generated<<"  %arc_coarse_dimensions = call %dx.types.Dimensions @dx.op.getDimensions(i32 72, %dx.types.Handle %arc_coarse_extent_handle, i32 undef)\n"
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
        if(out.group_shared){
            for(unsigned axis=0;axis<2;++axis){const std::string a=axis?"y":"x",extent=axis?"height":"width";
                const auto original="  %arc_coarse_rate_"+a+" = select i1 %arc_coarse_enabled_"+a+", i32 %arc_coarse_requested_safe_"+a+", i32 1";
                std::ostringstream guard;guard<<"  %arc_group_requested_"<<a<<" = select i1 %arc_coarse_enabled_"<<a<<", i32 %arc_coarse_requested_safe_"<<a<<", i32 1\n"
                    <<"  %arc_group_region_"<<a<<" = mul i32 %arc_group_requested_"<<a<<", "<<out.threads[axis]<<"\n"
                    <<"  %arc_group_remainder_"<<a<<" = urem i32 %arc_coarse_"<<extent<<", %arc_group_region_"<<a<<"\n"
                    <<"  %arc_group_complete_"<<a<<" = icmp eq i32 %arc_group_remainder_"<<a<<", 0\n"
                    <<"  %arc_coarse_rate_"<<a<<" = select i1 %arc_group_complete_"<<a<<", i32 %arc_group_requested_"<<a<<", i32 1";
                const auto at=transformed.find(original);if(at==transformed.npos)return reject("group_control_shape");transformed.replace(at,original.size(),guard.str());
            }
            const std::string old="%arc_coarse_active = and i1 %arc_coarse_group_active, %arc_coarse_bounded";const auto at=transformed.find(old);if(at==transformed.npos)return reject("group_guard_shape");transformed.replace(at,old.size(),"%arc_coarse_active = and i1 %arc_coarse_group_active, true");
        }

        // Replace original thread-id definitions, leaving the prelude calls
        // alone. All original instructions consequently use remapped pixels.
        for(unsigned d=0;d<2;++d)for(const auto& original_id:thread_ids[d]){
            const auto id=std::regex_replace(original_id,local,"%arc_coarse_orig_$1");
            for(std::size_t i=begin+1;i<end;++i){const auto c=code(lines[i]);if(!c.starts_with(id+" = "))continue;
                const auto pos=transformed.find(lines[i]);transformed.replace(pos,lines[i].size(),id+" = add i32 %arc_coarse_coord_"+(d?"y":"x")+", 0");}
        }
        unsigned serial{};std::size_t store_cursor{};
        std::map<std::string,std::string> predecessors;
        std::string current_block=entry_label;std::size_t block_cursor=begin+1;
        for(const auto& s:stores){
            for(;block_cursor<s.line;++block_cursor){const auto c=code(lines[block_cursor]);if(c.ends_with(':'))current_block=c.substr(0,c.size()-1);}
            const auto original=lines[s.line];const auto pos=transformed.find(original,store_cursor);if(pos==transformed.npos)return reject("store_rewrite");
            std::ostringstream expanded;expanded<<original<<'\n';
            for(unsigned y=0;y<(runtime_control?4:y_rate);++y)for(unsigned x=0;x<(runtime_control?4:x_rate);++x){if(!x&&!y)continue;
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
