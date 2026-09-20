#include "generic_optimizer.hpp"
#include "generic_binding_admission.hpp"
#include "generic_gpu_control.hpp"
#include "generic_command_mirror.hpp"
#include "generic_gpu_profile.hpp"
#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <tuple>
#include <cmath>

namespace arc::dx12::optimizer {
namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
struct Root {std::uint64_t id{};ID3D12RootSignature* native{};std::vector<std::byte> bytes;std::shared_ptr<const binding::Layout> layout;};
struct VariantStats {std::uint64_t id{},attempts{},admitted{},uniform_attempts{},uniform_proven{},uniform_steps{},uniform_known_reads{},uniform_unknown_reads{};double uniform_cpu_ms{};std::string last_reason,uniform_reason;UINT binding_class{},binding_register{},binding_space{};};
using UniformReadKey=std::tuple<unsigned,unsigned,unsigned>;
struct Variant:VariantStats {Ptr<ID3D12RootSignature> root;Ptr<ID3D12PipelineState> pipeline;shader::Transform contract;std::shared_ptr<const shader::UniformAccessProgram> access;shader::ResourceUsage cached_usage;std::map<UniformReadKey,shader::UniformWords> cached_reads;};
struct Pipeline {std::uint64_t id{},profile_id{};Ptr<ID3D12Device> device;std::shared_ptr<Root> root;std::vector<std::byte> code;std::shared_ptr<Variant> variant;std::string reason{"queued"};};
struct Use {std::shared_ptr<Variant> variant;binding::Arguments arguments;std::vector<std::uint64_t> heaps;UINT x{},y{},z{};};
struct Recording {
    ID3D12PipelineState* pipeline{};std::shared_ptr<Root> root;binding::Arguments arguments;
    std::vector<std::uint64_t> heaps;std::shared_ptr<GpuControl> control;std::vector<Use> uses;
    bool valid{true},closed{},epilogue{},render_pass{};
};
struct State {
    std::recursive_mutex mutex;std::condition_variable_any changed;
    std::atomic<bool> enabled{};UINT x_rate{1},y_rate{1},comparison_taps{},zero_factor{},mip_steps{};
    bool heaviest_only{};std::uint64_t selected_pipeline{},cost_session{},cost_prepared{};double selected_cost{};
    bool protect_edges{},instrumentation{true};float edge_threshold{.08f};
    std::filesystem::path worker,compiler,cache;
    std::map<ID3D12RootSignature*,std::shared_ptr<Root>> roots;
    std::map<ID3D12PipelineState*,std::shared_ptr<Pipeline>> pipelines;
    std::map<ID3D12GraphicsCommandList*,Recording> commands;
    std::map<ID3D12DescriptorHeap*,binding::DescriptorHeap> heaps;
    std::map<std::uint64_t,binding::DescriptorHeap> heaps_by_id;
    std::map<ID3D12Resource*,std::uint64_t> resource_ids;
    std::map<std::uint64_t,ID3D12Resource*> resource_natives;
    std::map<ID3D12Heap*,std::uint64_t> allocation_heaps;
    std::map<std::uint64_t,binding::Allocation> allocations;
    binding::BufferIndex buffers;
    DescriptorLedger descriptors;
    std::deque<std::shared_ptr<Pipeline>> jobs;
    std::vector<std::shared_ptr<GpuControl>> controls;
    std::map<std::uint64_t,VariantStats> history;
    std::uint64_t next{1},prepared{},declined{},modified_dispatches{},coarse_submissions{},neutral_submissions{},faults{},pool_misses{};
    std::string last_error;std::map<std::string,std::uint64_t> admission_reasons;
};
State& state(){static auto* s=new State;return *s;}
std::atomic<std::uint64_t> cpu_ns{};
thread_local unsigned metering_depth{};
struct CpuMeter {
    using Clock=std::chrono::steady_clock;
    bool outer{metering_depth++==0};Clock::time_point start;
    CpuMeter(){if(outer)start=Clock::now();}
    ~CpuMeter(){--metering_depth;if(outer)cpu_ns.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count()),std::memory_order_relaxed);}
};
void check(HRESULT value){if(FAILED(value))throw std::runtime_error("Optimizer HRESULT "+std::to_string(value));}
template<class F>void safe(F&& action)noexcept{CpuMeter meter;try{std::lock_guard lock(state().mutex);action();}catch(const std::exception& error){std::lock_guard lock(state().mutex);auto& s=state();++s.faults;s.x_rate=s.y_rate=1;s.comparison_taps=0;s.zero_factor=0;s.last_error=error.what();}catch(...){std::lock_guard lock(state().mutex);++state().faults;state().x_rate=state().y_rate=1;state().comparison_taps=0;state().zero_factor=0;state().last_error="unknown exception";}}
constexpr GUID lifetime_guid{0x109dd36a,0xb6f3,0x4f98,{0x89,0x7c,0xf3,0x2c,0xa2,0x60,0xb5,0xb4}};
class Lifetime final:public IUnknown {
    std::atomic<ULONG> count{1};void* object;unsigned kind;
public:
    Lifetime(void* p,unsigned k):object(p),kind(k){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown)return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++count;}
    ULONG STDMETHODCALLTYPE Release()override{const auto n=--count;if(!n){safe([&]{auto& s=state();
        if(kind==1)s.roots.erase(static_cast<ID3D12RootSignature*>(object));
        else if(kind==2)s.pipelines.erase(static_cast<ID3D12PipelineState*>(object));
        else if(kind==3)s.commands.erase(static_cast<ID3D12GraphicsCommandList*>(object));
        else if(kind==4){auto it=s.heaps.find(static_cast<ID3D12DescriptorHeap*>(object));if(it!=s.heaps.end()){s.descriptors.retire_heap(it->second.id);s.heaps_by_id.erase(it->second.id);s.heaps.erase(it);}}
        else if(kind==5){auto it=s.resource_ids.find(static_cast<ID3D12Resource*>(object));if(it!=s.resource_ids.end()){s.buffers.retire(it->second);s.resource_natives.erase(it->second);s.allocations.erase(it->second);s.resource_ids.erase(it);}}
        else if(kind==6)s.allocation_heaps.erase(static_cast<ID3D12Heap*>(object));
    });delete this;}return n;}
};
bool track(ID3D12Object* object,unsigned kind){auto* token=new Lifetime(object,kind);const auto result=object->SetPrivateDataInterface(lifetime_guid,token);token->Release();return SUCCEEDED(result);}
std::wstring environment(const wchar_t* name){const auto size=GetEnvironmentVariableW(name,nullptr,0);if(!size||size>32768)return {};std::wstring value(size,L'\0');const auto read=GetEnvironmentVariableW(name,value.data(),size);if(!read||read>=size)return {};value.resize(read);return value;}
std::wstring quote(const std::wstring& value){std::wstring out=L"\"";unsigned slashes=0;for(auto c:value){if(c==L'\\'){++slashes;continue;}if(c==L'"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}out.append(slashes*2,L'\\');return out+L'"';}
std::uint64_t resource_identity(ID3D12Resource* resource){
    if(!resource)return 0;auto& s=state();if(auto it=s.resource_ids.find(resource);it!=s.resource_ids.end())return it->second;
    if(s.resource_ids.size()>=16384)return 0;mirror::InternalCall guard;
    const auto id=s.next++;binding::Allocation a;a.id=id;a.description=resource->GetDesc();
    if(a.description.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER)a.gpu_address=resource->GetGPUVirtualAddress();
    if(!track(resource,5))return 0;s.resource_ids[resource]=id;s.resource_natives[id]=resource;s.allocations[id]=a;s.buffers.observe(a);return id;
}
void write_view(D3D12_CPU_DESCRIPTOR_HANDLE handle,DescriptorValue value){if(!state().descriptors.write(handle.ptr,value))state().descriptors.forget(handle.ptr);}
Recording* recording(ID3D12GraphicsCommandList* native){const auto it=state().commands.find(native);return it==state().commands.end()?nullptr:&it->second;}
void replay_arguments(ID3D12GraphicsCommandList* list,const binding::Arguments& arguments){
    const auto& layout=arguments.layout();if(!layout)return;
    for(UINT i=0;i<layout->parameters.size();++i){const auto a=arguments.raw_argument(i);if(!a)continue;
        if(a->type==D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){for(UINT word=0;word<layout->parameters[i].constants;++word)if(a->written&(UINT64(1)<<word))list->SetComputeRoot32BitConstant(i,a->words[word],word);continue;}
        if(!a->initialized)continue;
        switch(a->type){
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:list->SetComputeRootDescriptorTable(i,{a->address});break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:list->SetComputeRootConstantBufferView(i,a->address);break;
        case D3D12_ROOT_PARAMETER_TYPE_SRV:list->SetComputeRootShaderResourceView(i,a->address);break;
        case D3D12_ROOT_PARAMETER_TYPE_UAV:list->SetComputeRootUnorderedAccessView(i,a->address);break;
        default:break;
        }
    }
}
struct UniformMemory {
    Ptr<ID3D12Resource> resource;void* mapped{};UINT64 offset{},bytes{};
    binding::Argument constants;bool inline_constants{},null_view{};
    ~UniformMemory(){if(resource&&mapped){D3D12_RANGE no_writes{};resource->Unmap(0,&no_writes);}}
};
shader::ResourceUsage uniform_usage(const Use& use,const std::vector<binding::DescriptorHeap>& heaps){
    mirror::InternalCall internal;
    std::map<std::pair<unsigned,unsigned>,UniformMemory> views;
    auto read_memory=[&](unsigned range,unsigned reg,unsigned offset){
        shader::UniformWords result;auto& s=state();
        const auto declaration=std::find_if(use.variant->contract.resources.begin(),use.variant->contract.resources.end(),[&](const auto& c){return c.resource_class==2&&c.range_id==range&&reg>=c.shader_register&&reg-c.shader_register<c.count;});
        if(declaration==use.variant->contract.resources.end())return result;
        const auto key=std::pair{range,reg};auto [entry,inserted]=views.try_emplace(key);auto& view=entry->second;
        if(inserted){
            const auto location=use.arguments.locate(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,reg,declaration->space,D3D12_SHADER_VISIBILITY_ALL);
            if(!location)return result;const auto argument=use.arguments.argument(location->parameter);if(!argument)return result;
            view.bytes=declaration->kind;
            if(location->constants){view.constants=*argument;view.inline_constants=true;view.bytes=std::min<UINT64>(view.bytes,use.arguments.layout()->parameters[location->parameter].constants*4);}
            else{
                std::uint64_t resource{};
                if(location->table){
                    const binding::DescriptorHeap* heap=nullptr;
                    for(const auto& h:heaps)if(h.type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV&&h.gpu&&h.stride&&argument->address>=h.gpu&&
                        (argument->address-h.gpu)%h.stride==0&&(argument->address-h.gpu)/h.stride<h.count){if(heap)return result;heap=&h;}
                    if(!heap)return result;const auto index=(argument->address-heap->gpu)/heap->stride+location->table_offset;
                    if(index>=heap->count||index>(UINT64_MAX-heap->cpu)/heap->stride)return result;
                    const auto descriptor=s.descriptors.read(heap->cpu+index*heap->stride);
                    if(!descriptor||!descriptor->shape.known||descriptor->kind!=6)return result;
                    resource=descriptor->resource;view.offset=descriptor->shape.byte_offset;view.bytes=std::min<UINT64>(view.bytes,descriptor->shape.byte_size);view.null_view=!resource;
                }else{
                    if(const auto* a=s.buffers.resolve(s.allocations,argument->address,declaration->kind)){resource=a->id;view.offset=argument->address-a->gpu_address;}
                }
                if(resource){
                    const auto object=s.resource_natives.find(resource),dummy=s.resource_natives.end();if(object==dummy){view.bytes=0;return result;}
                    view.resource=object->second;D3D12_HEAP_PROPERTIES properties{};D3D12_HEAP_FLAGS flags{};
                    // A DEFAULT buffer may have GPU producers earlier in this
                    // very recording. Reading last-frame CPU data is not proof.
                    if(FAILED(view.resource->GetHeapProperties(&properties,&flags))||properties.Type!=D3D12_HEAP_TYPE_UPLOAD){view.bytes=0;return result;}
                    const auto description=view.resource->GetDesc();if(view.offset>description.Width){view.bytes=0;return result;}view.bytes=std::min<UINT64>(view.bytes,description.Width-view.offset);
                    D3D12_RANGE read_range{static_cast<SIZE_T>(view.offset),static_cast<SIZE_T>(view.offset+view.bytes)};
                    if(FAILED(view.resource->Map(0,&read_range,&view.mapped)))view.bytes=0;
                }
            }
        }
        for(unsigned component=0;component<4;++component){const auto byte=std::uint64_t(offset)+component*4;
            if(byte>view.bytes||view.bytes-byte<4)continue;
            if(view.inline_constants){const auto word=static_cast<unsigned>(byte/4);if(word<64&&(view.constants.written&(UINT64(1)<<word))){result.words[component]=view.constants.words[word];result.valid_mask|=1u<<component;}}
            else if(view.null_view){result.valid_mask|=1u<<component;}
            else if(view.mapped){std::memcpy(&result.words[component],static_cast<const char*>(view.mapped)+view.offset+byte,4);result.valid_mask|=1u<<component;}
        }
        return result;
    };
    std::map<UniformReadKey,shader::UniformWords> reads;
    auto reader=[&](unsigned range,unsigned reg,unsigned offset){const UniformReadKey key{range,reg,offset};if(auto it=reads.find(key);it!=reads.end())return it->second;const auto words=read_memory(range,reg,offset);reads.emplace(key,words);return words;};
    auto& variant=*use.variant;
    if(variant.cached_usage.complete){
        bool matches=true;
        for(const auto& [key,words]:variant.cached_reads){const auto [range,reg,offset]=key;if(!(reader(range,reg,offset)==words)){matches=false;break;}}
        if(matches){auto result=variant.cached_usage;result.steps=0;return result;}
    }
    auto result=variant.access->evaluate(reader);
    for(const auto& [key,words]:reads){(void)key;if(words.valid_mask)++variant.uniform_known_reads;else ++variant.uniform_unknown_reads;}
    // All pixel-dependent branches were explored. Identical values for every
    // observed uniform read therefore preserve this overapproximation. Descriptor
    // contents/allocations are still resolved afresh by the caller afterwards.
    if(result.complete){variant.cached_usage=result;variant.cached_reads=std::move(reads);}else{variant.cached_usage={};variant.cached_reads.clear();}
    return result;
}
std::shared_ptr<Variant> prepare_variant(const std::shared_ptr<Pipeline>& pipeline){
    auto& s=state();const auto source=s.cache/(std::to_string(pipeline->id)+".source.bin"),binary=s.cache/(std::to_string(pipeline->id)+".controlled.bin");
    {std::ofstream file(source,std::ios::binary);file.write(reinterpret_cast<const char*>(pipeline->code.data()),pipeline->code.size());file.close();if(!file)throw std::runtime_error("shader source cache IO");}
    std::set<UINT> spaces;for(const auto& p:pipeline->root->layout->parameters){if(p.type==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){for(const auto& r:p.ranges)spaces.insert(r.space);}else spaces.insert(p.space);}for(const auto& sampler:pipeline->root->layout->samplers)spaces.insert(sampler.RegisterSpace);
    UINT space=0;while(spaces.contains(space)&&space<65536)++space;if(space==65536)throw std::runtime_error("control register space capacity");
    auto command=quote(s.worker.wstring())+L" controlled:"+std::to_wstring(space)+L" "+quote(source.wstring())+L" "+quote(binary.wstring())+L" "+quote(s.compiler.wstring());
    STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
    if(!CreateProcessW(s.worker.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,s.cache.c_str(),&start,&process))throw std::runtime_error("shader worker launch");
    CloseHandle(process.hThread);const auto waited=WaitForSingleObject(process.hProcess,20000);DWORD code=1;
    if(waited!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,2);WaitForSingleObject(process.hProcess,1000);}else GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);
    if(code)throw std::runtime_error("shader_class_not_admitted_or_compiler_failed");
    auto manifest=binary;manifest+=L".contract";std::ifstream description(manifest);std::string tag;description>>tag;
    auto result=std::make_shared<Variant>();result->id=pipeline->id;auto& contract=result->contract;std::size_t count{};
    description>>contract.control_space>>contract.threads[0]>>contract.threads[1]>>contract.threads[2]>>contract.stores>>count>>contract.comparison_filter_groups>>contract.zero_factor_regions>>contract.edge_input_mask>>contract.mip_samples;
    if(!description||tag!="ARC_SHADER_CONTRACT_5"||count>128||contract.control_space==UINT32_MAX)throw std::runtime_error("shader worker contract");
    for(std::size_t i=0;i<count;++i){shader::ResourceContract r;description>>r.resource_class>>r.range_id>>r.shader_register>>r.space>>r.count>>r.kind;contract.resources.push_back(r);}
    if(!description)throw std::runtime_error("truncated shader contract");contract.admitted=true;
    auto access_path=binary;access_path+=L".access.ll";
    if(std::filesystem::is_regular_file(access_path)&&std::filesystem::file_size(access_path)<=8*1024*1024){std::ifstream access(access_path,std::ios::binary);std::string ir{std::istreambuf_iterator<char>(access),{}};result->access=shader::UniformAccessProgram::compile(ir);}
    const auto root=binding::append_control_cbv(pipeline->root->bytes,contract.control_space);if(root.empty())throw std::runtime_error("root_cannot_add_control");
    const auto size=std::filesystem::file_size(binary);if(!size||size>8*1024*1024)throw std::runtime_error("shader worker output size");
    std::ifstream input(binary,std::ios::binary);std::vector<char> bytes{std::istreambuf_iterator<char>(input),{}};
    if(bytes.size()!=size)throw std::runtime_error("shader worker output IO");
    mirror::InternalCall internal;check(pipeline->device->CreateRootSignature(0,root.data(),root.size(),IID_PPV_ARGS(&result->root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=result->root.Get();p.CS={bytes.data(),bytes.size()};check(pipeline->device->CreateComputePipelineState(&p,IID_PPV_ARGS(&result->pipeline)));
    return result;
}
DWORD WINAPI worker(void*){
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    for(;;){std::shared_ptr<Pipeline> job;{
        auto& s=state();std::unique_lock lock(s.mutex);s.changed.wait(lock,[&]{return !s.jobs.empty();});job=std::move(s.jobs.front());s.jobs.pop_front();}
        try{
            auto variant=prepare_variant(job);std::vector<std::shared_ptr<GpuControl>> controls;
            bool needs_pool=false;{std::lock_guard lock(state().mutex);needs_pool=std::none_of(state().controls.begin(),state().controls.end(),[&](const auto& c){return c->device()==job->device.Get();});}
            if(needs_pool){if(state().controls.size()+8>64)throw std::runtime_error("control pool capacity");mirror::InternalCall internal;for(unsigned i=0;i<8;++i)controls.push_back(std::make_shared<GpuControl>(job->device.Get()));}
            std::lock_guard lock(state().mutex);job->variant=std::move(variant);job->reason="prepared_neutral";job->code.clear();job->code.shrink_to_fit();++state().prepared;state().controls.insert(state().controls.end(),controls.begin(),controls.end());
        }catch(const std::exception& error){std::lock_guard lock(state().mutex);job->reason=error.what();job->code.clear();job->code.shrink_to_fit();++state().declined;}
    }
}
}
bool enabled()noexcept{return state().enabled.load(std::memory_order_relaxed);}
std::uint64_t cpu_nanoseconds()noexcept{return cpu_ns.load(std::memory_order_relaxed);}
bool initialize()noexcept{
    bool result=true;safe([&]{auto& s=state();if(s.enabled)return;
        const auto worker_path=environment(L"ARC_OPTIMIZER_WORKER");if(worker_path.empty())return;
        s.worker=worker_path;s.compiler=environment(L"ARC_OPTIMIZER_COMPILER");s.cache=environment(L"ARC_OPTIMIZER_CACHE");
        if(!s.worker.is_absolute()||!s.compiler.is_absolute()||!s.cache.is_absolute()||!std::filesystem::is_regular_file(s.worker)||!std::filesystem::is_regular_file(s.compiler)) {result=false;return;}
        s.cache/=std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());std::filesystem::create_directories(s.cache);
        HANDLE thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(!thread){result=false;return;}CloseHandle(thread);s.enabled=true;
    });return result;
}
bool configure(const wchar_t* input)noexcept{if(!input||!enabled())return false;bool accepted=false;safe([&]{auto& s=state();
    std::wstring command=input;bool heaviest=false;const auto separator=command.find(L'|');
    if(separator!=command.npos){if(command.substr(separator+1)!=L"heaviest")return;heaviest=true;command.resize(separator);}
    float edge_threshold=.08f;const auto parameter=command.find(L'@');
    if(parameter!=command.npos){std::size_t used{};const auto text=command.substr(parameter+1);edge_threshold=std::stof(text,&used);if(used!=text.size()||!std::isfinite(edge_threshold)||edge_threshold<0||edge_threshold>2)return;command.resize(parameter);}
    const auto* value=command.c_str();
    if(wcscmp(value,L"off")==0||wcscmp(value,L"neutral")==0){s.x_rate=s.y_rate=1;accepted=true;}
    else if(wcscmp(value,L"2x1")==0){s.x_rate=2;s.y_rate=1;accepted=true;}
    else if(wcscmp(value,L"1x2")==0){s.x_rate=1;s.y_rate=2;accepted=true;}
    else if(wcscmp(value,L"2x2")==0){s.x_rate=s.y_rate=2;accepted=true;}
    else if(wcscmp(value,L"pcf9")==0){s.x_rate=s.y_rate=1;s.comparison_taps=9;accepted=true;}
    else if(wcscmp(value,L"zero")==0){s.x_rate=s.y_rate=1;s.zero_factor=1;accepted=true;}
    else if(wcscmp(value,L"adaptive-1x2")==0){s.x_rate=1;s.y_rate=2;accepted=true;}
    else if(wcscmp(value,L"adaptive-2x2")==0){s.x_rate=s.y_rate=2;accepted=true;}
    else if(command==L"mip-half"||command==L"mip1"||command==L"mip2"){s.x_rate=s.y_rate=1;s.mip_steps=command==L"mip-half"?1:command==L"mip1"?2:4;accepted=true;}
    if(accepted&&wcscmp(value,L"pcf9")!=0)s.comparison_taps=0;
    if(accepted&&wcscmp(value,L"zero")!=0)s.zero_factor=0;
    if(accepted&&command!=L"mip-half"&&command!=L"mip1"&&command!=L"mip2")s.mip_steps=0;
    if(accepted){s.instrumentation=command!=L"off";s.protect_edges=command.starts_with(L"adaptive-");s.edge_threshold=edge_threshold;s.heaviest_only=heaviest;
        if(heaviest&&std::none_of(s.pipelines.begin(),s.pipelines.end(),[&](const auto& p){return p.second->id==s.selected_pipeline&&p.second->variant;})){s.selected_pipeline=0;s.cost_session=0;s.selected_cost=0;s.cost_prepared=0;}}
});return accepted;}
DescriptorWrite descriptor_write()noexcept{DescriptorWrite result;if(enabled())result.lock=std::unique_lock(state().mutex);return result;}
void root_created(ID3D12RootSignature* native,const void* bytes,SIZE_T size)noexcept{if(!enabled()||!native||!bytes||!size||size>65536)return;safe([&]{auto& s=state();if(s.roots.size()>=1024)return;auto root=std::make_shared<Root>();root->id=s.next++;root->native=native;const auto* data=static_cast<const std::byte*>(bytes);root->bytes.assign(data,data+size);root->layout=std::make_shared<binding::Layout>(binding::Layout::parse(root->bytes));if(track(native,1))s.roots[native]=std::move(root);});}
void compute_created(ID3D12PipelineState* native,const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc)noexcept{if(!enabled()||!native||!desc||desc->NodeMask>1||!desc->CS.pShaderBytecode||!desc->CS.BytecodeLength||desc->CS.BytecodeLength>2*1024*1024)return;const auto profile_id=gpu_profile::pipeline_identity(native);safe([&]{auto& s=state();if(s.pipelines.size()>=128||s.jobs.size()>=64)return;const auto root=s.roots.find(desc->pRootSignature);if(root==s.roots.end()||!root->second->layout->complete)return;
    auto p=std::make_shared<Pipeline>();p->id=s.next++;p->profile_id=profile_id;p->root=root->second;check(native->GetDevice(IID_PPV_ARGS(&p->device)));if(p->device->GetNodeCount()!=1)return;
    const auto* bytes=static_cast<const std::byte*>(desc->CS.pShaderBytecode);p->code.assign(bytes,bytes+desc->CS.BytecodeLength);if(!track(native,2))return;s.pipelines[native]=p;s.jobs.push_back(std::move(p));s.changed.notify_one();
});}
void heap_created(ID3D12DescriptorHeap* native)noexcept{if(!enabled()||!native)return;safe([&]{auto& s=state();if(s.heaps.contains(native)||s.heaps.size()>=4096)return;mirror::InternalCall guard;const auto desc=native->GetDesc();Ptr<ID3D12Device> device;check(native->GetDevice(IID_PPV_ARGS(&device)));
    binding::DescriptorHeap h{s.next++,native->GetCPUDescriptorHandleForHeapStart().ptr,0,device->GetDescriptorHandleIncrementSize(desc.Type),desc.NumDescriptors,desc.Type};if(desc.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)h.gpu=native->GetGPUDescriptorHandleForHeapStart().ptr;
    if(!s.descriptors.register_heap(h.id,h.cpu,h.stride,h.count))return;if(!track(native,4)){s.descriptors.retire_heap(h.id);return;}s.heaps[native]=h;s.heaps_by_id[h.id]=h;
});}
void resource_created(ID3D12Resource* native,ID3D12Heap* heap,UINT64 offset)noexcept{if(!enabled()||!native)return;safe([&]{auto& s=state();const auto id=resource_identity(native);if(!id)return;auto& a=s.allocations.at(id);a.kind=heap?binding::AllocationKind::Placed:binding::AllocationKind::Committed;
    if(heap){auto it=s.allocation_heaps.find(heap);if(it==s.allocation_heaps.end()){if(s.allocation_heaps.size()>=4096||!track(heap,6)){a.kind=binding::AllocationKind::Unknown;return;}it=s.allocation_heaps.emplace(heap,s.next++).first;}a.heap=it->second;a.offset=offset;}
    Ptr<ID3D12Device> device;check(native->GetDevice(IID_PPV_ARGS(&device)));a.bytes=device->GetResourceAllocationInfo(0,1,&a.description).SizeInBytes;if(a.bytes==UINT64_MAX)a.kind=binding::AllocationKind::Unknown;
});}
void srv(ID3D12Resource* resource,const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{if(!enabled())return;safe([&]{DescriptorValue v;v.kind=1;v.resource=resource_identity(resource);if(desc){v.shape.known=!resource||v.resource;v.shape.format=desc->Format;v.shape.dimension=desc->ViewDimension;v.shape.component_mapping=desc->Shader4ComponentMapping;
    switch(desc->ViewDimension){case D3D12_SRV_DIMENSION_TEXTURE2D:v.first_mip=desc->Texture2D.MostDetailedMip;v.mips=desc->Texture2D.MipLevels;v.shape.plane=desc->Texture2D.PlaneSlice;break;
    case D3D12_SRV_DIMENSION_BUFFER:v.shape.first_element=desc->Buffer.FirstElement;v.shape.elements=desc->Buffer.NumElements;v.shape.stride=desc->Buffer.StructureByteStride;v.shape.flags=desc->Buffer.Flags;break;
    default:break;}}
    write_view(handle,v);
});}
void uav(ID3D12Resource* resource,ID3D12Resource* counter,const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{if(!enabled())return;safe([&]{DescriptorValue v;v.kind=2;v.resource=resource_identity(resource);v.shape.counter_resource=resource_identity(counter);if(desc){v.shape.known=(!resource||v.resource)&&(!counter||v.shape.counter_resource);v.shape.format=desc->Format;v.shape.dimension=desc->ViewDimension;
    if(desc->ViewDimension==D3D12_UAV_DIMENSION_TEXTURE2D){v.first_mip=desc->Texture2D.MipSlice;v.mips=1;v.shape.plane=desc->Texture2D.PlaneSlice;}}
    write_view(handle,v);
});}
void cbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept {
    if(!enabled())return;safe([&]{DescriptorValue v;v.kind=6;
        if(desc){v.shape.byte_size=desc->SizeInBytes;v.shape.known=desc->BufferLocation==0;
            if(const auto* a=state().buffers.resolve(state().allocations,desc->BufferLocation,desc->SizeInBytes)){v.resource=a->id;v.shape.byte_offset=desc->BufferLocation-a->gpu_address;v.shape.known=true;}}
        write_view(handle,v);
    });
}
void sampler(const D3D12_SAMPLER_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{if(!enabled())return;safe([&]{DescriptorValue v;v.kind=5;v.shape.known=desc!=nullptr;write_view(handle,v);});}
void copy_descriptors(UINT count,D3D12_CPU_DESCRIPTOR_HANDLE destination,D3D12_CPU_DESCRIPTOR_HANDLE source,UINT stride)noexcept{if(!enabled())return;safe([&]{auto& ledger=state().descriptors;if(!stride||count>65536)throw std::runtime_error("descriptor copy capacity");std::vector<std::optional<DescriptorValue>> values;values.reserve(count);for(UINT i=0;i<count;++i)values.push_back(ledger.read(source.ptr+UINT64(i)*stride));for(UINT i=0;i<count;++i){const auto address=destination.ptr+UINT64(i)*stride;if(values[i]){if(!ledger.write(address,*values[i]))ledger.forget(address);}else ledger.forget(address);}});}
void descriptor_ranges(UINT dc,const D3D12_CPU_DESCRIPTOR_HANDLE* dst,const UINT* ds,UINT sc,const D3D12_CPU_DESCRIPTOR_HANDLE* src,const UINT* ss,UINT stride)noexcept{if(!enabled())return;safe([&]{if(dc>1024||sc>1024||!stride||(!dst&&dc)||(!src&&sc))throw std::runtime_error("descriptor ranges unavailable");std::vector<std::uint64_t> addresses;std::vector<std::optional<DescriptorValue>> values;auto& ledger=state().descriptors;
    for(UINT r=0;r<dc;++r){const auto count=ds?ds[r]:1;if(count>65536||addresses.size()+count>65536)throw std::runtime_error("descriptor range capacity");for(UINT i=0;i<count;++i)addresses.push_back(dst[r].ptr+UINT64(i)*stride);}
    for(UINT r=0;r<sc;++r){const auto count=ss?ss[r]:1;if(count>65536||values.size()+count>65536)throw std::runtime_error("descriptor range capacity");for(UINT i=0;i<count;++i)values.push_back(ledger.read(src[r].ptr+UINT64(i)*stride));}
    for(std::size_t i=0;i<addresses.size();++i){if(addresses.size()==values.size()&&values[i]){if(!ledger.write(addresses[i],*values[i]))ledger.forget(addresses[i]);}else ledger.forget(addresses[i]);}
});}
void begin(ID3D12GraphicsCommandList* native,ID3D12PipelineState* pipeline)noexcept{if(!enabled()||!native)return;safe([&]{auto& s=state();auto it=s.commands.find(native);if(it==s.commands.end()){if(s.commands.size()>=256||!track(native,3))return;it=s.commands.emplace(native,Recording{}).first;}it->second=Recording{};it->second.pipeline=pipeline;});}
void pipeline(ID3D12GraphicsCommandList* native,ID3D12PipelineState* pipeline)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native))c->pipeline=pipeline;});}
void heaps(ID3D12GraphicsCommandList* native,UINT count,ID3D12DescriptorHeap*const* heaps)noexcept{if(!enabled())return;safe([&]{auto* c=recording(native);if(!c)return;if(count>2||(!heaps&&count)){c->valid=false;return;}std::vector<std::uint64_t> next;for(UINT i=0;i<count;++i){heap_created(heaps[i]);const auto it=state().heaps.find(heaps[i]);if(it==state().heaps.end()){c->valid=false;return;}next.push_back(it->second.id);}if(next!=c->heaps){c->arguments.invalidate_tables();c->heaps=std::move(next);}});}
void root(ID3D12GraphicsCommandList* native,ID3D12RootSignature* root)noexcept{if(!enabled())return;safe([&]{auto* c=recording(native);if(!c)return;const auto it=state().roots.find(root);c->root=it==state().roots.end()?nullptr:it->second;c->arguments.signature(c->root?c->root->id:0,c->root?c->root->layout:nullptr);});}
void table(ID3D12GraphicsCommandList* native,UINT parameter,D3D12_GPU_DESCRIPTOR_HANDLE handle)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native))c->arguments.table(parameter,handle);});}
void constants(ID3D12GraphicsCommandList* native,UINT parameter,UINT count,const UINT* values,UINT offset)noexcept{if(!enabled())return;safe([&]{if((!values&&count)||count>64)return;if(auto* c=recording(native))c->arguments.constants(parameter,offset,{values,count});});}
void descriptor(ID3D12GraphicsCommandList* native,UINT parameter,D3D12_ROOT_PARAMETER_TYPE type,UINT64 address)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native))c->arguments.descriptor(parameter,type,address);});}
void invalidate(ID3D12GraphicsCommandList* native)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native))c->valid=false;});}
void state_unknown(ID3D12GraphicsCommandList* native)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native)){c->arguments.reset();c->root.reset();c->pipeline=nullptr;}});}
void render_pass(ID3D12GraphicsCommandList* native,bool begin,D3D12_RENDER_PASS_FLAGS flags)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native)){c->render_pass=begin;if(flags&(D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS|D3D12_RENDER_PASS_FLAG_RESUMING_PASS))c->valid=false;}});}
void invalidate_all()noexcept{if(!enabled())return;safe([&]{state().x_rate=state().y_rate=1;state().comparison_taps=0;state().zero_factor=0;for(auto& [native,c]:state().commands){(void)native;c.valid=false;}});}
bool dispatch(ID3D12GraphicsCommandList* native,UINT x,UINT y,UINT z)noexcept{if(!enabled())return false;bool changed=false;safe([&]{auto& s=state();auto* c=recording(native);if(!c||!c->valid||c->closed||c->render_pass||!c->root||c->uses.size()>=GpuControl::capacity)return;const auto p=s.pipelines.find(c->pipeline);if(p==s.pipelines.end()||!p->second->variant||p->second->root!=c->root)return;
    if(!s.instrumentation||(s.heaviest_only&&p->second->id!=s.selected_pipeline))return;
    const auto variant=p->second->variant;
    if(!c->control){for(auto& control:s.controls)if(control.use_count()==1&&control->device()==p->second->device.Get()&&control->ready()){control->keep_alive.clear();c->control=control;break;}if(!c->control){++s.pool_misses;return;}}
    const UINT slot=static_cast<UINT>(c->uses.size());c->uses.push_back({variant,c->arguments,c->heaps,x,y,z});c->control->keep_alive.push_back(variant);
    mirror::InternalCall internal;native->SetComputeRootSignature(variant->root.Get());replay_arguments(native,c->arguments);native->SetComputeRootConstantBufferView(static_cast<UINT>(c->root->layout->parameters.size()),c->control->address(slot));native->SetPipelineState(variant->pipeline.Get());native->Dispatch(x,y,z);
    native->SetComputeRootSignature(c->root->native);replay_arguments(native,c->arguments);native->SetPipelineState(c->pipeline);changed=true;++s.modified_dispatches;
});return changed;}
void close(ID3D12GraphicsCommandList* native)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native)){if(c->control&&c->valid&&!c->closed&&!c->render_pass&&!c->uses.empty()){mirror::InternalCall internal;c->control->record_neutralize(native);c->epilogue=true;}c->closed=true;}});}
bool execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists)noexcept {
    if(!enabled()||!queue||(!lists&&count))return false;
    CpuMeter meter;
    auto& s=state();std::lock_guard lock(s.mutex);
    std::array<GpuControl*,64> controls{};unsigned used=0;
    auto fault=[&](const char* message){++s.faults;s.x_rate=s.y_rate=1;s.comparison_taps=0;s.zero_factor=0;try{s.last_error=message;}catch(...) {}};
    auto remember=[&](GpuControl* c){for(unsigned j=0;j<used;++j)if(controls[j]==c)return false;if(used>=controls.size()){fault("control capacity");return false;}controls[used++]=c;return true;};
    if(count>1024){
        // An oversized native batch still executes exactly once. Conservatively
        // order every retained control, with neutral policy and bounded storage.
        for(auto& c:s.controls)if(remember(c.get()))try{mirror::InternalCall internal;c->prepare(queue,{});}catch(const std::exception& e){fault(e.what());}
    }else for(UINT i=0;i<count;++i){
        const auto found=s.commands.find(reinterpret_cast<ID3D12GraphicsCommandList*>(lists[i]));if(found==s.commands.end())continue;
        auto& c=found->second;if(!c.control||!remember(c.control.get()))continue;
        try{
            std::array<ControlValue,GpuControl::capacity> values{};
            if(!s.faults&&c.valid&&c.closed&&c.epilogue)for(unsigned n=0;n<c.uses.size();++n){
                const auto& use=c.uses[n];std::vector<binding::DescriptorHeap> heaps;
                for(auto id:use.heaps){const auto h=s.heaps_by_id.find(id);if(h!=s.heaps_by_id.end())heaps.push_back(h->second);}
                auto admitted=binding::admit_compute(use.arguments,use.variant->contract,s.descriptors,heaps,s.allocations,use.x,use.y,use.z,nullptr,&s.buffers);
                if(!admitted.admitted&&admitted.reason=="unknown_descriptor"&&admitted.binding_class==0&&use.variant->access){
                    auto& stats=*use.variant;const auto started=std::chrono::steady_clock::now();++stats.uniform_attempts;
                    const auto usage=uniform_usage(use,heaps);stats.uniform_steps+=usage.steps;stats.uniform_reason=usage.reason;
                    stats.uniform_cpu_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
                    if(usage.complete){++stats.uniform_proven;admitted=binding::admit_compute(use.arguments,use.variant->contract,s.descriptors,heaps,s.allocations,use.x,use.y,use.z,&usage,&s.buffers);}
                }
                auto& v=*use.variant;++v.attempts;v.admitted+=admitted.admitted;v.last_reason=admitted.reason;v.binding_class=admitted.binding_class;v.binding_register=admitted.binding_register;v.binding_space=admitted.binding_space;
                if(s.history.size()<256||s.history.contains(v.id))s.history[v.id]=v;
                if(s.admission_reasons.size()<64||s.admission_reasons.contains(admitted.reason))++s.admission_reasons[admitted.reason];
                if(admitted.admitted&&(!s.heaviest_only||use.variant->id==s.selected_pipeline)){
                    values[n]={s.x_rate,s.y_rate,admitted.width,admitted.height,use.variant->contract.comparison_filter_groups?s.comparison_taps:0,use.variant->contract.zero_factor_regions?s.zero_factor:0};
                    if(use.variant->contract.mip_samples)values[n].mip_steps=s.mip_steps;
                    if(s.protect_edges){unsigned mask=0,selected=0;
                        for(const auto& input:admitted.inputs){const auto& r=input.contract;const auto& d=input.allocation.description;const auto& v=input.view;
                            if(selected>=4||r.resource_class!=0||r.range_id>=31||!(use.variant->contract.edge_input_mask&(1u<<r.range_id))||v.shape.dimension!=D3D12_SRV_DIMENSION_TEXTURE2D||v.first_mip>=d.MipLevels||v.first_mip>=32)continue;
                            if(std::max<UINT64>(1,d.Width>>v.first_mip)==admitted.width&&std::max(1u,d.Height>>v.first_mip)==admitted.height){mask|=1u<<r.range_id;++selected;}
                        }
                        if(mask){values[n].edge_sources=0x80000000u|mask;values[n].edge_threshold=s.edge_threshold;}else values[n]=ControlValue{};
                    }
                }
            }
            mirror::InternalCall internal;auto* helper=c.control->prepare(queue,{values.data(),c.uses.size()});
            if(helper){queue->ExecuteCommandLists(1,&helper);++s.coarse_submissions;}else ++s.neutral_submissions;
        }catch(const std::exception& e){fault(e.what());try{mirror::InternalCall internal;c.control->prepare(queue,{});}catch(...) {}}
        catch(...){fault("submission preparation failed");try{mirror::InternalCall internal;c.control->prepare(queue,{});}catch(...) {}}
    }
    // Never return false after submitting a helper: the original batch and all
    // retirement signals remain owned by this function even on preparation error.
    {mirror::InternalCall internal;queue->ExecuteCommandLists(count,lists);
        for(unsigned i=0;i<used;++i)try{controls[i]->submitted(queue);}catch(const std::exception& e){fault(e.what());}}
    return true;
}
void collect()noexcept{
    if(!enabled())return;bool need_costs=false;
    safe([&]{auto& s=state();for(auto& control:s.controls)control->release_completed_queue();need_costs=s.heaviest_only&&(!s.selected_pipeline||s.cost_prepared!=s.prepared);});
    if(!need_costs)return;
    const auto costs=gpu_profile::compute_costs();
    safe([&]{auto& s=state();if(!s.heaviest_only||costs.empty())return;double best=0;std::uint64_t selected=0;
        for(const auto& cost:costs){auto p=s.pipelines.find(cost.pipeline);if(p==s.pipelines.end()||!p->second->variant||p->second->profile_id!=cost.pipeline_identity)continue;
            if(cost.total_gpu_ms>best){best=cost.total_gpu_ms;selected=p->second->id;}}
        s.selected_pipeline=selected;s.selected_cost=best;s.cost_session=costs.front().session;s.cost_prepared=s.prepared;
    });
}
void snapshot(std::ostream& out){std::lock_guard lock(state().mutex);const auto& s=state();out<<"{\"enabled\":"<<(s.enabled?"true":"false")<<",\"automatic_quality_admission\":false,\"selected_pipeline\":"<<s.selected_pipeline<<",\"selected_profile_cost_ms\":"<<s.selected_cost<<",\"prepared\":"<<s.prepared<<",\"declined\":"<<s.declined<<",\"pending\":"<<s.jobs.size()<<",\"modified_dispatches\":"<<s.modified_dispatches<<",\"coarse_submissions\":"<<s.coarse_submissions<<",\"neutral_submissions\":"<<s.neutral_submissions<<",\"pool_misses\":"<<s.pool_misses<<",\"faults\":"<<s.faults<<",\"last_error\":"<<std::quoted(s.last_error)<<",\"admission\":{";bool first=true;for(const auto& [reason,count]:s.admission_reasons){if(!first)out<<',';first=false;out<<std::quoted(reason)<<':'<<count;}out<<"},\"variants\":[";first=true;for(const auto& [id,v]:s.history){(void)id;if(!first)out<<',';first=false;out<<"{\"id\":"<<v.id<<",\"attempts\":"<<v.attempts<<",\"admitted\":"<<v.admitted<<",\"uniform_attempts\":"<<v.uniform_attempts<<",\"uniform_proven\":"<<v.uniform_proven<<",\"uniform_steps\":"<<v.uniform_steps<<",\"uniform_cpu_ms\":"<<v.uniform_cpu_ms<<",\"uniform_known_reads\":"<<v.uniform_known_reads<<",\"uniform_unknown_reads\":"<<v.uniform_unknown_reads<<",\"uniform_reason\":"<<std::quoted(v.uniform_reason)<<",\"last_reason\":"<<std::quoted(v.last_reason)<<",\"binding\":["<<v.binding_class<<','<<v.binding_register<<','<<v.binding_space<<"]}";}out<<"]}";}
}
