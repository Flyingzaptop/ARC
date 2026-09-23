#include "generic_pixel_optimizer.hpp"
#include "generic_binding_state.hpp"
#include "generic_gpu_control.hpp"
#include "generic_command_mirror.hpp"
#include "generic_cpu_workers.hpp"
#include "generic_background_budget.hpp"
#include "arc/candidate_contract.hpp"
#include <wrl/client.h>
#include <atomic>
#include <map>
#include <vector>
#include <deque>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <cstring>
namespace arc::dx12::pixel {
namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
using Control=optimizer::GpuControl;
struct Root {ID3D12RootSignature* native{};std::vector<std::byte> bytes;std::shared_ptr<binding::Layout> layout;};
struct Variant {unsigned mips{},filters{},samples{},rays{};Ptr<ID3D12RootSignature> root;Ptr<ID3D12PipelineState> pipeline;std::unique_ptr<arc::CandidateRecord> record;arc::PublishedCandidate published;};
struct Pipeline {std::uint64_t id{},captured_size{},retry_after_ms{};arc::CandidateFingerprint fingerprint{};Ptr<ID3D12PipelineState> original;Ptr<ID3D12RootSignature> original_root;Ptr<ID3D12Device> device;std::shared_ptr<Root> root;D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};std::array<std::vector<char>,5> code;std::vector<D3D12_INPUT_ELEMENT_DESC> elements;std::vector<std::string> names;std::shared_ptr<Variant> variant;bool attempted{};std::string reason;};
struct Command {ID3D12PipelineState* pipeline{};std::shared_ptr<Root> root;binding::Arguments args;std::vector<std::uintptr_t> heaps;std::shared_ptr<Control> control;bool mips{},filters{},samples{};bool valid{true},closed{},modified{},bound{};};
struct QueuedJob {std::shared_ptr<Pipeline> pipeline;arc::DiscoveryBudget::JobLease lease;std::uint64_t epoch{};arc::CandidateFingerprint fingerprint{};};
struct State {std::recursive_mutex mutex;std::condition_variable_any wake;std::atomic<unsigned> steps{},filter_taps{},sample_percent{100};std::atomic<bool> discovery_enabled{true};std::atomic<std::uint64_t> discovery_epoch{1};bool worker_started{};std::filesystem::path worker,compiler,cache;std::map<ID3D12RootSignature*,std::shared_ptr<Root>> roots;std::map<ID3D12PipelineState*,std::shared_ptr<Pipeline>> pipelines;std::map<ID3D12GraphicsCommandList*,Command> commands;std::vector<std::shared_ptr<Control>> controls;std::deque<QueuedJob> jobs;std::uint64_t next{},captured_bytes{},prepared{},prepared_mips{},prepared_filters{},prepared_samples{},prepared_rays{},declined{},recorded_draws{},modified_submissions{},faults{},pool_misses{};std::string error;};
State& state(){static auto* s=new State;return *s;}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("pixel HRESULT "+std::to_string(hr));}
template<class F>void safe(F&& f)noexcept{try{std::lock_guard lock(state().mutex);f();}catch(const std::exception& e){std::lock_guard lock(state().mutex);state().steps=0;state().filter_taps=0;state().sample_percent=100;++state().faults;state().error=e.what();}catch(...){state().steps=0;state().filter_taps=0;state().sample_percent=100;}}
constexpr GUID life_guid{0x50334612,0x5879,0x4d38,{0x92,0x80,0x29,0x17,0x99,0x01,0x18,0x04}};
class Lifetime:public IUnknown {std::atomic<ULONG> refs{1};void* pointer;bool command;public:Lifetime(void* p,bool c):pointer(p),command(c){}HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(id!=IID_IUnknown)return E_NOINTERFACE;*out=this;AddRef();return S_OK;}ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n){safe([&]{if(command)state().commands.erase(static_cast<ID3D12GraphicsCommandList*>(pointer));else state().roots.erase(static_cast<ID3D12RootSignature*>(pointer));});delete this;}return n;}};
bool track(ID3D12Object* object,bool command){auto* token=new Lifetime(object,command);auto hr=object->SetPrivateDataInterface(life_guid,token);token->Release();return SUCCEEDED(hr);}
std::wstring env(const wchar_t* name){wchar_t value[32768]{};const auto n=GetEnvironmentVariableW(name,value,32768);return n&&n<32768?std::wstring(value,n):L"";}
std::wstring quote(const std::wstring& s){std::wstring out=L"\"";unsigned n=0;for(auto c:s){if(c==L'\\'){++n;continue;}if(c==L'"'){out.append(n*2+1,L'\\');out+=c;}else{out.append(n,L'\\');out+=c;}n=0;}out.append(n*2,L'\\');return out+L'"';}
struct Cancelled {};
bool current(const QueuedJob& job) noexcept {auto& s=state();return job.lease.valid()&&s.discovery_enabled.load(std::memory_order_acquire)&&s.discovery_epoch.load(std::memory_order_acquire)==job.epoch;}
std::uint64_t code_identity(const std::vector<char>& code) noexcept {std::uint64_t hash=14695981039346656037ull;for(unsigned char byte:code){hash^=byte;hash*=1099511628211ull;}return hash?hash:1;}
std::shared_ptr<Variant> republish(const std::shared_ptr<Pipeline>& p){
    const auto& old=*p->variant;
    auto next=std::make_shared<Variant>();next->mips=old.mips;next->filters=old.filters;next->samples=old.samples;next->rays=old.rays;next->root=old.root;next->pipeline=old.pipeline;
    next->record=std::make_unique<arc::CandidateRecord>(*old.record);
    next->record->fingerprint=p->fingerprint;
    const auto admission=arc::admit_candidate(*next->record);
    if(!admission.accepted()||!next->published.mark_analyzed()||!next->published.mark_eligible()||!next->published.prepare(admission)||!next->published.local_validate(true)||!next->published.activate())throw std::runtime_error("pixel republication declined");
    return next;
}
std::shared_ptr<Variant> prepare(const QueuedJob& queued){
    if(!current(queued))throw Cancelled{};
    const auto& p=queued.pipeline;
    auto& s=state();unsigned space=0;for(;;++space){bool used=false;for(const auto& a:p->root->layout->parameters){if(a.type==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){for(const auto& r:a.ranges)used|=r.space==space;}else used|=a.space==space;}for(const auto& a:p->root->layout->samplers)used|=a.RegisterSpace==space;if(!used)break;if(space>=65535)throw std::runtime_error("pixel control space");}
    const auto augmented=binding::append_control_cbv(p->root->bytes,space);if(augmented.empty())throw std::runtime_error("pixel root budget");
    std::filesystem::create_directories(s.cache);const auto source=s.cache/(std::to_string(p->id)+".source"),output=s.cache/(std::to_string(p->id)+".dxil");
    {std::ofstream f(source,std::ios::binary);f.write(p->code[1].data(),p->code[1].size());if(!f)throw std::runtime_error("pixel source write");}
    std::unique_lock gate(background_compute_gate(),std::defer_lock);
    while(!gate.try_lock_for(std::chrono::milliseconds(50)))if(!current(queued))throw Cancelled{};
    if(!current(queued))throw Cancelled{};
    auto command=quote(s.worker.wstring())+L" pixel-controlled:"+std::to_wstring(space)+L" "+quote(source.wstring())+L" "+quote(output.wstring())+L" "+quote(s.compiler.wstring());
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION child{};
    if(!CreateProcessW(s.worker.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED|BELOW_NORMAL_PRIORITY_CLASS,nullptr,nullptr,&startup,&child))throw std::runtime_error("pixel compiler launch");
    struct ProcessJob {HANDLE h{CreateJobObjectW(nullptr,nullptr)};~ProcessJob(){if(h)CloseHandle(h);}} job;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job.h||!SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits))||!AssignProcessToJobObject(job.h,child.hProcess)){TerminateProcess(child.hProcess,1);CloseHandle(child.hThread);CloseHandle(child.hProcess);throw std::runtime_error("pixel compiler ownership");}
    cpu_cost::Registration meter(cpu_cost::Kind::Compiler,child.hProcess,child.hThread);const auto resumed=ResumeThread(child.hThread);CloseHandle(child.hThread);DWORD code=1;
    bool cancelled=false;const auto deadline=GetTickCount64()+20000;
    if(resumed!=DWORD(-1))for(;;){if(!current(queued)){cancelled=true;break;}const auto result=WaitForSingleObject(child.hProcess,50);if(result==WAIT_OBJECT_0){GetExitCodeProcess(child.hProcess,&code);break;}if(result!=WAIT_TIMEOUT||GetTickCount64()>=deadline)break;}
    if(cancelled||code){TerminateProcess(child.hProcess,1);WaitForSingleObject(child.hProcess,50);}CloseHandle(child.hProcess);
    if(cancelled)throw Cancelled{};if(code)throw std::runtime_error("pixel shader declined or compiler failed");
    if(!current(queued))throw Cancelled{};
    if(!std::filesystem::is_regular_file(output)||std::filesystem::file_size(output)>4*1024*1024)throw std::runtime_error("pixel binary size");
    std::ifstream f(output,std::ios::binary);std::vector<char> binary{std::istreambuf_iterator<char>(f),{}};if(binary.empty())throw std::runtime_error("pixel binary empty");
    mirror::InternalCall internal;auto result=std::make_shared<Variant>();auto contract=output;contract+=L".pixel-contract";std::ifstream manifest(contract);std::string tag;manifest>>tag>>result->mips>>result->filters>>result->samples>>result->rays;if(!manifest||tag!="ARC_PIXEL_QUALITY_1"||!(result->mips||result->filters||result->samples||result->rays))throw std::runtime_error("pixel quality contract");check(p->device->CreateRootSignature(0,augmented.data(),augmented.size(),IID_PPV_ARGS(&result->root)));auto desc=p->desc;desc.pRootSignature=result->root.Get();desc.PS={binary.data(),binary.size()};desc.CachedPSO={};check(p->device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&result->pipeline)));
    if(!current(queued))throw Cancelled{};
    std::vector<std::shared_ptr<Control>> added;bool need_pool=false;{std::lock_guard lock(s.mutex);need_pool=s.controls.empty();}if(need_pool)for(unsigned i=0;i<8;++i)added.push_back(std::make_shared<Control>(p->device.Get(),false,false));{std::lock_guard lock(s.mutex);if(s.controls.empty())s.controls=std::move(added);}
    return result;
}
DWORD WINAPI worker(void*){cpu_cost::Registration meter;SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);auto& s=state();for(;;){QueuedJob job;{std::unique_lock lock(s.mutex);s.wake.wait(lock,[&]{return !s.jobs.empty();});job=std::move(s.jobs.front());s.jobs.pop_front();}auto p=job.pipeline;try{
    if(!current(job))throw Cancelled{};
    auto variant=prepare(job);
    arc::CandidateRecord record;record.id=p->id;record.analyzer_version=1;record.backend=arc::CandidateBackend::Dx12;record.kind=arc::CandidateKind::ApproximateShader;record.exactness=arc::CandidateExactness::Approximate;record.fingerprint=job.fingerprint;record.completeness=arc::EvidenceCompleteness::ProvenUnderAssumptions;record.required_facts=record.proven_facts=1;record.required_guards=3;record.guard_boundary=arc::GuardBoundary::Dx12Record;record.guard_before_effects=record.supported_scope=true;record.experimental_unverified=true;record.execution_lifetime=arc::ExecutionLifetime::GpuExternalFence;record.original_entry=reinterpret_cast<std::uint64_t>(p->original.Get());record.variant_entry=reinterpret_cast<std::uint64_t>(variant->pipeline.Get());record.supported_model="DX12 graphics PSO with parsed root signature and controlled pixel shader";record.assumptions="root identity and command state are rechecked before draw; variant lifetime follows GPU control fence retention";record.input_effect_scope="graphics root signature, pixel PSO, temporary control CBV; original state restored after draw";
    const auto admission=arc::admit_candidate(record);
    if(!admission.accepted()||!variant->published.mark_analyzed()||!variant->published.mark_eligible()||!variant->published.prepare(admission)||!variant->published.local_validate(true))throw std::runtime_error("pixel candidate admission");
    variant->record=std::make_unique<arc::CandidateRecord>(std::move(record));
    std::lock_guard lock(s.mutex);
    if(!current(job)){p->attempted=false;p->reason="cancelled";continue;}
    if(!variant->published.activate())throw Cancelled{};
    p->variant=std::move(variant);p->reason="ready";++s.prepared;s.prepared_mips+=p->variant->mips!=0;s.prepared_filters+=p->variant->filters!=0;s.prepared_samples+=p->variant->samples!=0;s.prepared_rays+=p->variant->rays!=0;
}catch(const Cancelled&){std::lock_guard lock(s.mutex);p->attempted=false;p->reason="cancelled";}catch(const std::exception& e){std::lock_guard lock(s.mutex);if(!current(job)){p->attempted=false;p->reason="cancelled";}else{p->reason=e.what();++s.declined;}}}}
bool initialize(){auto& s=state();if(s.worker_started)return true;s.worker=env(L"ARC_OPTIMIZER_WORKER");s.compiler=env(L"ARC_OPTIMIZER_COMPILER");const auto cache=env(L"ARC_OPTIMIZER_CACHE");if(s.worker.empty()||s.compiler.empty()||cache.empty()||!s.worker.is_absolute()||!s.compiler.is_absolute()||!std::filesystem::path(cache).is_absolute())return false;s.cache=std::filesystem::path(cache)/(L"pixel-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));HANDLE thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(!thread)return false;CloseHandle(thread);s.worker_started=true;return true;}
void replay(ID3D12GraphicsCommandList* list,const binding::Arguments& args){const auto& layout=args.layout();for(UINT i=0;i<layout->parameters.size();++i){const auto* a=args.raw_argument(i);if(!a||!a->observed)continue;switch(a->type){case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:list->SetGraphicsRootDescriptorTable(i,{a->address});break;case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:for(UINT word=0;word<layout->parameters[i].constants;++word)if(a->written&(UINT64(1)<<word))list->SetGraphicsRoot32BitConstant(i,a->words[word],word);break;case D3D12_ROOT_PARAMETER_TYPE_CBV:list->SetGraphicsRootConstantBufferView(i,a->address);break;case D3D12_ROOT_PARAMETER_TYPE_SRV:list->SetGraphicsRootShaderResourceView(i,a->address);break;case D3D12_ROOT_PARAMETER_TYPE_UAV:list->SetGraphicsRootUnorderedAccessView(i,a->address);break;}}}
}
void root_created(ID3D12RootSignature* native,const void* data,SIZE_T bytes)noexcept{if(!native||!data||!bytes||bytes>65536)return;safe([&]{auto& s=state();if(s.roots.contains(native)||s.roots.size()>=512)return;auto root=std::make_shared<Root>();root->native=native;const auto* b=static_cast<const std::byte*>(data);root->bytes.assign(b,b+bytes);root->layout=std::make_shared<binding::Layout>(binding::Layout::parse(root->bytes));if(root->layout->complete&&track(native,false))s.roots[native]=std::move(root);});}
void created(ID3D12PipelineState* native,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc)noexcept{
    if(!native||!desc||!desc->PS.BytecodeLength||desc->StreamOutput.NumEntries||desc->StreamOutput.NumStrides)return;
    safe([&]{auto& s=state();if(!initialize()||s.pipelines.contains(native)||s.pipelines.size()>=64)return;auto root=s.roots.find(desc->pRootSignature);if(root==s.roots.end())return;SIZE_T size=0;for(auto code:{desc->VS,desc->PS,desc->DS,desc->HS,desc->GS}){if(code.BytecodeLength>2*1024*1024)return;size+=code.BytecodeLength;}if(s.captured_bytes+size>32*1024*1024||desc->InputLayout.NumElements>64)return;
        auto p=std::make_shared<Pipeline>();p->id=++s.next;p->original=native;p->original_root=desc->pRootSignature;p->root=root->second;p->desc=*desc;p->desc.CachedPSO={};check(native->GetDevice(IID_PPV_ARGS(&p->device)));if(p->device->GetNodeCount()!=1)return;
        D3D12_SHADER_BYTECODE* destinations[]{&p->desc.VS,&p->desc.PS,&p->desc.DS,&p->desc.HS,&p->desc.GS};D3D12_SHADER_BYTECODE codes[]{desc->VS,desc->PS,desc->DS,desc->HS,desc->GS};
        for(unsigned i=0;i<5;++i){if(!codes[i].BytecodeLength)continue;const auto* bytes=static_cast<const char*>(codes[i].pShaderBytecode);p->code[i].assign(bytes,bytes+codes[i].BytecodeLength);*destinations[i]={p->code[i].data(),p->code[i].size()};}
        if(desc->InputLayout.NumElements){p->elements.assign(desc->InputLayout.pInputElementDescs,desc->InputLayout.pInputElementDescs+desc->InputLayout.NumElements);p->names.reserve(p->elements.size());for(auto& e:p->elements){if(!e.SemanticName||strnlen_s(e.SemanticName,256)==256)return;p->names.emplace_back(e.SemanticName);}for(unsigned i=0;i<p->elements.size();++i)p->elements[i].SemanticName=p->names[i].c_str();p->desc.InputLayout={p->elements.data(),UINT(p->elements.size())};}
        p->captured_size=size+p->root->bytes.size();
        p->fingerprint={code_identity(p->code[1]),p->id,0,
            reinterpret_cast<std::uint64_t>(p->root->native),0,
            reinterpret_cast<std::uint64_t>(p->device.Get())};
        s.captured_bytes+=size;s.pipelines[native]=std::move(p);
    });
}
void begin(ID3D12GraphicsCommandList* native,ID3D12PipelineState* pso)noexcept{safe([&]{auto& s=state();if(!s.worker_started||native->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;if(!s.commands.contains(native)){if(s.commands.size()>=4096||!track(native,true))return;}auto& c=s.commands[native];c=Command{};c.pipeline=pso;});}
void pipeline(ID3D12GraphicsCommandList* native,ID3D12PipelineState* pso)noexcept{safe([&]{if(auto i=state().commands.find(native);i!=state().commands.end())i->second.pipeline=pso;});}
void root(ID3D12GraphicsCommandList* native,ID3D12RootSignature* root)noexcept{safe([&]{auto& s=state();auto i=s.commands.find(native);if(i==s.commands.end())return;auto r=s.roots.find(root);i->second.root=r==s.roots.end()?nullptr:r->second;i->second.args.signature(reinterpret_cast<std::uint64_t>(root),r==s.roots.end()?nullptr:r->second->layout);});}
void table(ID3D12GraphicsCommandList* c,UINT p,D3D12_GPU_DESCRIPTOR_HANDLE h)noexcept{safe([&]{if(auto i=state().commands.find(c);i!=state().commands.end())i->second.args.table(p,h);});}
void constants(ID3D12GraphicsCommandList* c,UINT p,UINT n,const UINT* words,UINT offset)noexcept{safe([&]{if(auto i=state().commands.find(c);i!=state().commands.end()){if(n>64||(!words&&n))i->second.valid=false;else i->second.args.constants(p,offset,{words,n});}});}
void descriptor(ID3D12GraphicsCommandList* c,UINT p,D3D12_ROOT_PARAMETER_TYPE type,UINT64 address)noexcept{safe([&]{if(auto i=state().commands.find(c);i!=state().commands.end())i->second.args.descriptor(p,type,address);});}
void heaps(ID3D12GraphicsCommandList* c,UINT count,ID3D12DescriptorHeap*const* heaps)noexcept{safe([&]{if(auto i=state().commands.find(c);i!=state().commands.end()){if(count>2||(!heaps&&count)){i->second.valid=false;return;}std::vector<std::uintptr_t> next;for(UINT n=0;n<count;++n)next.push_back(reinterpret_cast<std::uintptr_t>(heaps[n]));std::sort(next.begin(),next.end());if(next!=i->second.heaps){if(!i->second.heaps.empty())i->second.valid=false;i->second.heaps=std::move(next);i->second.args.invalidate_tables();}}});}
void invalidate(ID3D12GraphicsCommandList* c)noexcept{safe([&]{if(auto i=state().commands.find(c);i!=state().commands.end())i->second.valid=false;});}
bool before_draw(ID3D12GraphicsCommandList* native)noexcept{if(!active())return false;bool changed=false;safe([&]{auto& s=state();if(!s.discovery_enabled.load(std::memory_order_acquire))return;auto i=s.commands.find(native);if(i==s.commands.end())return;auto& c=i->second;if(!c.valid||c.closed||!c.root)return;auto p=s.pipelines.find(c.pipeline);if(p==s.pipelines.end()||p->second->root!=c.root)return;
    if(!p->second->variant){if(!p->second->attempted&&s.jobs.size()<16&&GetTickCount64()>=p->second->retry_after_ms){auto reservation=background_discovery_budget().try_reserve(p->second->captured_size);if(reservation){s.jobs.push_back({p->second,std::move(*reservation),s.discovery_epoch.load(std::memory_order_acquire),p->second->fingerprint});p->second->attempted=true;s.wake.notify_one();}else p->second->retry_after_ms=GetTickCount64()+1000;}return;}
    // Direct lists start with undefined root arguments. All graphics setters
    // are tracked; bundles/indirect/unknown state changes invalidate the list.
    // Replay only defined words, preserving unused/undefined parameters rather
    // than requiring every declared root parameter to be used by the shader.
    auto lease=p->second->variant->published.try_acquire(p->second->fingerprint,3);
    if(!lease)return;
    if(!c.control){for(auto& control:s.controls)if(control.use_count()==1&&control->device()==p->second->device.Get()&&control->ready()){control->keep_alive.clear();control->begin_recording();c.control=control;break;}if(!c.control){++s.pool_misses;return;}}
    if(std::find(c.control->keep_alive.begin(),c.control->keep_alive.end(),p->second->variant)==c.control->keep_alive.end())c.control->keep_alive.push_back(p->second->variant);
    mirror::InternalCall internal;native->SetGraphicsRootSignature(p->second->variant->root.Get());replay(native,c.args);native->SetGraphicsRootConstantBufferView(UINT(c.root->layout->parameters.size()),c.control->address(0));native->SetPipelineState(p->second->variant->pipeline.Get());c.mips|=p->second->variant->mips!=0;c.filters|=p->second->variant->filters!=0;c.samples|=p->second->variant->samples!=0||p->second->variant->rays!=0;c.bound=c.modified=true;changed=true;++s.recorded_draws;
});return changed;}
void after_draw(ID3D12GraphicsCommandList* native)noexcept{safe([&]{auto i=state().commands.find(native);if(i==state().commands.end()||!i->second.bound)return;auto& c=i->second;mirror::InternalCall internal;native->SetGraphicsRootSignature(c.root->native);replay(native,c.args);native->SetPipelineState(c.pipeline);c.bound=false;});}
void close(ID3D12GraphicsCommandList* native)noexcept{safe([&]{auto i=state().commands.find(native);if(i==state().commands.end())return;auto& c=i->second;if(c.valid&&c.modified&&c.control){mirror::InternalCall internal;c.control->record_neutralize(native);}c.closed=true;});}
Submission before_submit(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists)noexcept{
    Submission out;try{auto& s=state();out.lock=std::unique_lock(s.mutex);mirror::InternalCall internal;
        for(UINT i=0;i<count;++i){auto it=s.commands.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));if(it==s.commands.end()||!it->second.control||!it->second.modified)continue;const auto& c=it->second;bool duplicate=false;for(unsigned j=0;j<out.count;++j)duplicate|=out.controls[j].get()==c.control.get();if(duplicate)continue;
            // This pixel-only control retains one Variant per distinct recorded
            // PSO (at most the 64 tracked pipelines). A cached list keeps its
            // original Variant after stop/resume; a newly published Variant
            // cannot authorize that old recording. Neutralize the whole list
            // if any recorded publication is invalid or its guard is unknown.
            bool admitted=c.valid&&c.closed&&s.discovery_enabled&&c.control->keep_alive.size()<=64;
            if(admitted)for(const auto& held:c.control->keep_alive){
                auto* variant=static_cast<Variant*>(held.get());
                if(!variant||!variant->record||!variant->published.try_acquire(variant->record->fingerprint,3)){admitted=false;break;}
            }
            if(c.control->keep_alive.empty())admitted=false;
            optimizer::ControlValue value;value.mip_steps=admitted&&c.mips?s.steps.load():0;value.comparison_taps=admitted&&c.filters?s.filter_taps.load():0;value.sample_percent=admitted&&c.samples?s.sample_percent.load():100;auto* helper=c.control->prepare(queue,{&value,1});if(helper){queue->ExecuteCommandLists(1,&helper);++s.modified_submissions;}out.controls[out.count++]=c.control;
        }
    }catch(const std::exception& e){state().steps=0;state().filter_taps=0;state().sample_percent=100;++state().faults;state().error=e.what();}return out;
}
void after_submit(Submission& ticket,ID3D12CommandQueue* queue)noexcept{try{mirror::InternalCall internal;for(unsigned i=0;i<ticket.count;++i)std::static_pointer_cast<Control>(ticket.controls[i])->submitted(queue);}catch(const std::exception& e){state().steps=0;state().filter_taps=0;state().sample_percent=100;++state().faults;state().error=e.what();}}
unsigned requested_steps()noexcept{return state().steps.load();}
std::array<unsigned,3> requested_budget()noexcept{auto& s=state();return {s.steps.load(),s.filter_taps.load(),s.sample_percent.load()};}
bool active()noexcept{auto& s=state();return s.steps||s.filter_taps||s.sample_percent!=100;}
bool configure(unsigned steps,unsigned taps,unsigned percent)noexcept{if(steps>8||(taps>25)||(percent<1||percent>100))return false;bool accepted=false;safe([&]{auto& s=state();if((steps||taps||percent!=100)&&!initialize())return;s.steps=steps;s.filter_taps=taps;s.sample_percent=percent;accepted=true;});return accepted;}
void set_discovery_enabled(bool enabled)noexcept{auto& s=state();std::lock_guard lock(s.mutex);if(s.discovery_enabled.load(std::memory_order_acquire)==enabled)return;
    s.discovery_epoch.fetch_add(1,std::memory_order_acq_rel);
    if(!enabled){s.discovery_enabled.store(false,std::memory_order_release);for(auto& job:s.jobs)job.pipeline->attempted=false;s.jobs.clear();for(auto& [key,p]:s.pipelines){(void)key;if(p->variant)p->variant->published.invalidate(arc::CandidateReason::UserStopped);}return;}
    try{for(auto& [key,p]:s.pipelines){(void)key;if(!p->variant)continue;++p->fingerprint.dependency_epoch;p->variant=republish(p);}s.discovery_enabled.store(true,std::memory_order_release);}catch(const std::exception& e){for(auto& [key,p]:s.pipelines){(void)key;if(p->variant)p->variant->published.invalidate(arc::CandidateReason::UserStopped);}s.error=e.what();++s.faults;}}
bool available()noexcept{bool result=false;safe([&]{result=!state().pipelines.empty()&&!state().faults;});return result;}
bool ready()noexcept{bool result=false;safe([&]{auto& s=state();if(!s.discovery_enabled||s.faults)return;result=std::any_of(s.pipelines.begin(),s.pipelines.end(),[](const auto& entry){return entry.second->variant&&entry.second->variant->published.lifecycle()==arc::CandidateLifecycle::Active;});});return result;}
bool restoration_ready()noexcept{bool result=false;safe([&]{result=!active()&&std::all_of(state().controls.begin(),state().controls.end(),[](const auto& c){return c->ready();});});return result;}
void snapshot(std::ostream& out){std::lock_guard lock(state().mutex);auto& s=state();out<<"{\"mip_half_steps\":"<<s.steps<<",\"comparison_taps\":"<<s.filter_taps<<",\"independent_sample_percent\":"<<s.sample_percent<<",\"discovery_enabled\":"<<(s.discovery_enabled?"true":"false")<<",\"queued_jobs\":"<<s.jobs.size()<<",\"tracked_pipelines\":"<<s.pipelines.size()<<",\"prepared\":"<<s.prepared<<",\"prepared_mip_variants\":"<<s.prepared_mips<<",\"prepared_shadow_variants\":"<<s.prepared_filters<<",\"prepared_sample_variants\":"<<s.prepared_samples<<",\"prepared_inline_ray_variants\":"<<s.prepared_rays<<",\"declined\":"<<s.declined<<",\"recorded_modified_draws\":"<<s.recorded_draws<<",\"modified_submissions\":"<<s.modified_submissions<<",\"pool_misses\":"<<s.pool_misses<<",\"captured_code_bytes\":"<<s.captured_bytes<<",\"faults\":"<<s.faults<<",\"last_error\":"<<std::quoted(s.error)<<"}";}
}
