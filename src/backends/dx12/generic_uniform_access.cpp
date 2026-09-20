#include "generic_uniform_access.hpp"
#include <algorithm>
#include <charconv>
#include <deque>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace arc::dx12::shader {
namespace {
struct Operand {unsigned value{};bool literal{};};
enum class Op {Handle,Load,Extract,Phi,Select,Add,Sub,Mul,And,Or,Xor,Shl,Lshr,Ashr,Udiv,Sdiv,Urem,Srem,Eq,Ne,Ult,Ule,Ugt,Uge,Slt,Sle,Sgt,Sge};
struct Instruction {Op op{};unsigned target{},extra{},extra2{},flags{};Operand a,b,c;std::vector<std::pair<unsigned,Operand>> incoming;};
struct Block {std::vector<Instruction> instructions;Operand condition;unsigned yes{},no{};bool returns{},conditional{},terminated{};std::vector<unsigned> live;};
std::string trim(std::string s){const auto b=s.find_first_not_of(" \t\r\n");return b==s.npos?std::string{}:s.substr(b,s.find_last_not_of(" \t\r\n")-b+1);}
unsigned integer(const std::string& s){long long n{};auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),n);if(e!=std::errc{}||p!=s.data()+s.size()||n<INT32_MIN||n>UINT32_MAX)throw std::runtime_error("integer");return static_cast<unsigned>(n);}
void operands(const Instruction& i,const std::function<void(Operand)>& use){
    switch(i.op){case Op::Handle:case Op::Extract:use(i.a);break;
    case Op::Load:use(i.a);use(i.b);break;
    case Op::Phi:for(auto [pred,a]:i.incoming){(void)pred;use(a);}break;
    case Op::Select:use(i.a);use(i.b);use(i.c);break;
    default:use(i.a);use(i.b);break;}
}
struct Value {std::array<unsigned,4> words{};unsigned mask{},kind{};}; // 1 scalar, 2 handle, 3 CB vector
Value scalar(unsigned n){Value v;v.words[0]=n;v.kind=1;v.mask=1;return v;}
bool known(const Value& v){return v.kind==1&&(v.mask&1);}
Value arithmetic(const Instruction& i,const Value& a,const Value& b){
    if(!known(a)||!known(b))return {};const unsigned x=a.words[0],y=b.words[0];const auto sx=static_cast<std::int32_t>(x),sy=static_cast<std::int32_t>(y);
    std::uint64_t wide{};std::int64_t signed_wide{};unsigned value{};bool math=false;
    switch(i.op){
    case Op::Add:wide=std::uint64_t(x)+y;signed_wide=std::int64_t(sx)+sy;value=x+y;math=true;break;
    case Op::Sub:wide=std::uint64_t(x)-y;signed_wide=std::int64_t(sx)-sy;value=x-y;math=true;break;
    case Op::Mul:wide=std::uint64_t(x)*y;signed_wide=std::int64_t(sx)*sy;value=x*y;math=true;break;
    case Op::And:value=x&y;break;case Op::Or:value=x|y;break;case Op::Xor:value=x^y;break;
    case Op::Shl:if(y>=32)return {};value=x<<y;break;case Op::Lshr:if(y>=32)return {};value=x>>y;break;case Op::Ashr:if(y>=32)return {};value=static_cast<unsigned>(sx>>y);break;
    case Op::Udiv:if(!y)return {};value=x/y;break;case Op::Urem:if(!y)return {};value=x%y;break;
    case Op::Sdiv:if(!sy||(sx==INT32_MIN&&sy==-1))return {};value=static_cast<unsigned>(sx/sy);break;
    case Op::Srem:if(!sy||(sx==INT32_MIN&&sy==-1))return {};value=static_cast<unsigned>(sx%sy);break;
    case Op::Eq:value=x==y;break;case Op::Ne:value=x!=y;break;
    case Op::Ult:value=x<y;break;case Op::Ule:value=x<=y;break;case Op::Ugt:value=x>y;break;case Op::Uge:value=x>=y;break;
    case Op::Slt:value=sx<sy;break;case Op::Sle:value=sx<=sy;break;case Op::Sgt:value=sx>sy;break;case Op::Sge:value=sx>=sy;break;
    default:return {};
    }
    if(math&&(((i.flags&1)&&wide>UINT32_MAX)||((i.flags&2)&&(signed_wide<INT32_MIN||signed_wide>INT32_MAX))))return {};
    return scalar(value);
}
struct KeyHash {std::size_t operator()(const std::vector<unsigned>& values)const noexcept{std::size_t h=1469598103934665603ull;for(auto v:values){h^=v;h*=1099511628211ull;}return h;}};
}
struct UniformAccessProgram::Impl {std::vector<Block> blocks;unsigned registers{};};
std::shared_ptr<const UniformAccessProgram> UniformAccessProgram::compile(std::string_view input){
    try{
        if(input.size()>8*1024*1024)return {};auto impl=std::make_shared<Impl>();impl->blocks.emplace_back();
        std::map<std::string,unsigned> registers,labels{{"0",0}};unsigned next_reg=1,current=0;bool inside=false,finished=false;
        auto reg=[&](const std::string& s){auto [it,inserted]=registers.emplace(s,next_reg);if(inserted)++next_reg;if(next_reg>8192)throw std::runtime_error("register capacity");return it->second;};
        auto operand=[&](const std::string& text){const auto s=trim(text);if(s=="undef"||s=="poison")return Operand{};if(s=="true")return Operand{1,true};if(s=="false")return Operand{0,true};if(s.starts_with('%'))return Operand{reg(s),false};return Operand{integer(s),true};};
        auto label=[&](const std::string& s){auto [it,inserted]=labels.emplace(s,static_cast<unsigned>(impl->blocks.size()));if(inserted)impl->blocks.emplace_back();if(impl->blocks.size()>2048)throw std::runtime_error("block capacity");return it->second;};
        const std::string atom=R"((%[A-Za-z0-9_.$]+|-?[0-9]+|true|false|undef|poison))";
        const std::regex block(R"(^; <label>:([0-9]+).*$)"),named_block(R"(^([A-Za-z0-9_.$]+):.*$)");
        const std::regex handle("^(%[A-Za-z0-9_.$]+) = call %dx.types.Handle @dx.op.createHandle\\(i32 57, i8 ([0-3]), i32 ([0-9]+), i32 "+atom+", i1 (?:true|false)\\).*$");
        const std::regex load("^(%[A-Za-z0-9_.$]+) = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32\\(i32 59, %dx.types.Handle "+atom+", i32 "+atom+"\\).*$");
        const std::regex extract("^(%[A-Za-z0-9_.$]+) = extractvalue %dx.types.CBufRet.i32 "+atom+", ([0-3])$");
        const std::regex binary("^(%[A-Za-z0-9_.$]+) = (add|sub|mul|and|or|xor|shl|lshr|ashr|udiv|sdiv|urem|srem)((?: (?:nsw|nuw|exact))*) i(?:32|1) "+atom+", "+atom+"$");
        const std::regex compare("^(%[A-Za-z0-9_.$]+) = icmp (eq|ne|ult|ule|ugt|uge|slt|sle|sgt|sge) i(?:32|1) "+atom+", "+atom+"$");
        const std::regex select("^(%[A-Za-z0-9_.$]+) = select i1 "+atom+", i(?:32|1) "+atom+", i(?:32|1) "+atom+"$");
        const std::regex phi(R"(^(%[A-Za-z0-9_.$]+) = phi i(?:32|1) (.*)$)"),incoming("\\[ "+atom+", %([A-Za-z0-9_.$]+) \\]");
        const std::regex branch("^br i1 "+atom+", label %([A-Za-z0-9_.$]+), label %([A-Za-z0-9_.$]+)$"),jump(R"(^br label %([A-Za-z0-9_.$]+)$)");
        const std::map<std::string,Op> operations={{"add",Op::Add},{"sub",Op::Sub},{"mul",Op::Mul},{"and",Op::And},{"or",Op::Or},{"xor",Op::Xor},{"shl",Op::Shl},{"lshr",Op::Lshr},{"ashr",Op::Ashr},{"udiv",Op::Udiv},{"sdiv",Op::Sdiv},{"urem",Op::Urem},{"srem",Op::Srem},{"eq",Op::Eq},{"ne",Op::Ne},{"ult",Op::Ult},{"ule",Op::Ule},{"ugt",Op::Ugt},{"uge",Op::Uge},{"slt",Op::Slt},{"sle",Op::Sle},{"sgt",Op::Sgt},{"sge",Op::Sge}};
        std::istringstream source{std::string(input)};std::string line;std::smatch m;unsigned instructions=0;
        while(std::getline(source,line)){line=trim(line);if(line.starts_with("define ")){if(inside||finished)return {};inside=true;continue;}if(!inside)continue;if(line=="}"){inside=false;finished=true;continue;}
            if(std::regex_match(line,m,block)||std::regex_match(line,m,named_block)){current=label(m[1].str());continue;}
            const auto comment=line.find(';');if(comment!=line.npos)line=trim(line.substr(0,comment));if(line.empty())continue;
            Instruction ins;bool added=true;
            if(std::regex_match(line,m,handle)){ins.op=Op::Handle;ins.target=reg(m[1]);ins.extra=integer(m[2]);ins.extra2=integer(m[3]);ins.a=operand(m[4]);}
            else if(std::regex_match(line,m,load)){ins.op=Op::Load;ins.target=reg(m[1]);ins.a=operand(m[2]);ins.b=operand(m[3]);}
            else if(std::regex_match(line,m,extract)){ins.op=Op::Extract;ins.target=reg(m[1]);ins.a=operand(m[2]);ins.extra=integer(m[3]);}
            else if(std::regex_match(line,m,binary)){ins.op=operations.at(m[2]);ins.target=reg(m[1]);const auto flags=m[3].str();ins.flags=(flags.find("nuw")!=flags.npos?1u:0u)|(flags.find("nsw")!=flags.npos?2u:0u);ins.a=operand(m[4]);ins.b=operand(m[5]);}
            else if(std::regex_match(line,m,compare)){ins.op=operations.at(m[2]);ins.target=reg(m[1]);ins.a=operand(m[3]);ins.b=operand(m[4]);}
            else if(std::regex_match(line,m,select)){ins.op=Op::Select;ins.target=reg(m[1]);ins.a=operand(m[2]);ins.b=operand(m[3]);ins.c=operand(m[4]);}
            else if(std::regex_match(line,m,phi)){ins.op=Op::Phi;ins.target=reg(m[1]);const auto pairs=m[2].str();for(auto it=std::sregex_iterator(pairs.begin(),pairs.end(),incoming);it!=std::sregex_iterator();++it)ins.incoming.emplace_back(label((*it)[2].str()),operand((*it)[1]));if(ins.incoming.empty()||ins.incoming.size()>256)return {};}
            else {added=false;
                if(std::regex_match(line,m,branch)){const auto condition=operand(m[1]);const auto yes=label(m[2]),no=label(m[3]);auto& b=impl->blocks[current];b.condition=condition;b.yes=yes;b.no=no;b.conditional=true;b.terminated=true;}
                else if(std::regex_match(line,m,jump)){const auto target=label(m[1]);auto& b=impl->blocks[current];b.yes=target;b.terminated=true;}
                else if(line=="ret void"){impl->blocks[current].returns=true;impl->blocks[current].terminated=true;}
                else if(line.starts_with("br ")||line.starts_with("switch ")||line.starts_with("invoke ")||line.starts_with("ret "))return {};
                // All other definitions remain unknown. In particular, pixel
                // samples, floating point math and thread IDs are not guessed.
            }
            if(added){impl->blocks[current].instructions.push_back(std::move(ins));if(++instructions>16384)return {};}
        }
        if(!finished||inside)return {};impl->registers=next_reg;
        for(const auto& b:impl->blocks)if(!b.terminated)return {};
        // Liveness makes paths differing only in dead registers join again.
        std::vector<std::set<unsigned>> uses(impl->blocks.size()),defs(impl->blocks.size()),live(impl->blocks.size());
        for(unsigned j=0;j<impl->blocks.size();++j){const auto& b=impl->blocks[j];auto add=[&](Operand a){if(!a.literal&&a.value&&!defs[j].contains(a.value))uses[j].insert(a.value);};for(const auto& i:b.instructions){operands(i,add);defs[j].insert(i.target);}if(b.conditional)add(b.condition);}
        bool changed=true;unsigned iteration=0;
        while(changed){if(++iteration>4096)return {};changed=false;for(std::size_t k=impl->blocks.size();k-->0;){const auto& b=impl->blocks[k];auto next=uses[k];if(!b.returns){for(auto r:live[b.yes])if(!defs[k].contains(r))next.insert(r);if(b.conditional)for(auto r:live[b.no])if(!defs[k].contains(r))next.insert(r);}if(next!=live[k]){live[k]=std::move(next);changed=true;}}}
        for(unsigned j=0;j<impl->blocks.size();++j){for(const auto& i:impl->blocks[j].instructions)if(i.op==Op::Phi)live[j].insert(i.target);impl->blocks[j].live.assign(live[j].begin(),live[j].end());}
        auto result=std::shared_ptr<UniformAccessProgram>(new UniformAccessProgram);result->impl_=std::move(impl);return result;
    }catch(const std::exception&){return {};}
}

ResourceUsage UniformAccessProgram::evaluate(const UniformReader& reader,unsigned budget)const {
    ResourceUsage out;if(!impl_||!reader||!budget){out.reason="no_uniform_program";return out;}
    struct State {unsigned block{},predecessor{UINT32_MAX};std::vector<Value> values;};
    std::vector<State> pending;pending.push_back({0,UINT32_MAX,std::vector<Value>(impl_->registers)});
    std::unordered_set<std::vector<unsigned>,KeyHash> visited;
    auto read=[](const State& s,Operand a){return a.literal?scalar(a.value):s.values[a.value];};
    while(!pending.empty()){
        if(pending.size()>256||visited.size()>4096){out.reason="uniform_state_budget";return out;}
        auto state=std::move(pending.back());pending.pop_back();const auto& block=impl_->blocks[state.block];++out.states;
        std::vector<std::pair<unsigned,Value>> phi;
        for(const auto& i:block.instructions)if(i.op==Op::Phi){Value value;for(const auto& [pred,a]:i.incoming)if(pred==state.predecessor){value=read(state,a);break;}phi.emplace_back(i.target,value);}
        for(const auto& [target,value]:phi)state.values[target]=value;
        std::vector<unsigned> key{state.block};key.reserve(1+block.live.size()*6);
        for(auto r:block.live){const auto& v=state.values[r];key.push_back(v.kind);key.push_back(v.mask);key.insert(key.end(),v.words.begin(),v.words.end());}
        if(!visited.insert(std::move(key)).second)continue;
        for(const auto& i:block.instructions){if(++out.steps>budget){out.reason="uniform_step_budget";return out;}if(i.op==Op::Phi)continue;
            Value v;const auto a=read(state,i.a),b=read(state,i.b);
            if(i.op==Op::Handle){v.kind=2;v.words={i.extra,i.extra2,a.words[0],0};v.mask=known(a)?1:0;auto& range=out.ranges[{i.extra,i.extra2}];if(known(a))range.indices.insert(a.words[0]);else range.all=true;}
            else if(i.op==Op::Load){v.kind=3;if(a.kind==2&&a.mask&&a.words[0]==2&&known(b)&&b.words[0]<=UINT32_MAX/16){const auto words=reader(a.words[1],a.words[2],b.words[0]*16);v.words=words.words;v.mask=words.valid_mask&15;}}
            else if(i.op==Op::Extract){if(a.kind==3&&(a.mask&(1u<<i.extra)))v=scalar(a.words[i.extra]);}
            else if(i.op==Op::Select){const auto yes=read(state,i.b),no=read(state,i.c);if(known(a))v=a.words[0]?yes:no;else if(yes.kind==no.kind&&yes.mask==no.mask&&yes.words==no.words)v=yes;}
            else v=arithmetic(i,a,b);
            state.values[i.target]=v;
        }
        if(block.returns)continue;
        const auto condition=read(state,block.condition);const unsigned previous=state.block;
        if(block.conditional&&!known(condition)){auto other=state;other.predecessor=previous;other.block=block.no;pending.push_back(std::move(other));state.block=block.yes;}
        else state.block=block.conditional?(condition.words[0]?block.yes:block.no):block.yes;
        state.predecessor=previous;pending.push_back(std::move(state));
    }
    out.complete=true;out.reason="uniform_access_overapproximation";return out;
}
}
