#include "arc/uniform_scene_guard.hpp"
#include "generic_performance_proposal.hpp"
#include <dxgi1_6.h>
#include "generic_spatial_probe.hpp"
#include "arc/spatial_sensitivity.hpp"
#include <bit>
#include "generic_background_budget.hpp"
#include "generic_optimizer.hpp"
#include "generic_binding_admission.hpp"
#include "generic_gpu_control.hpp"
#include "generic_command_mirror.hpp"
#include "generic_gpu_profile.hpp"
#include "generic_shader_cache.hpp"
#include "arc/intercept_cpu_meter.hpp"
#include "arc/policy_binding_evidence.hpp"
#include "generic_cpu_workers.hpp"
#include "json.hpp"
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
#include <source_location>
#include <unordered_map>

namespace arc::dx12::optimizer {
namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
struct Root {std::uint64_t id{};ID3D12RootSignature* native{};std::vector<std::byte> bytes;std::shared_ptr<const binding::Layout> layout;};
struct AllocationCache {Ptr<ID3D12Device> device;std::map<std::array<UINT64,11>,UINT64> sizes;};
struct VariantStats {std::uint64_t id{},attempts{},admitted{},uniform_attempts{},uniform_proven{},uniform_steps{},uniform_known_reads{},uniform_unknown_reads{};double uniform_cpu_ms{};std::string last_reason,uniform_reason;UINT binding_class{},binding_register{},binding_space{};};
using UniformReadKey=std::tuple<unsigned,unsigned,unsigned>;
struct Variant:VariantStats {std::string analysis_key;Ptr<ID3D12RootSignature> root,probe_root;Ptr<ID3D12PipelineState> pipeline,spatial_pipeline,probe_pipeline;shader::Transform contract;std::shared_ptr<const shader::UniformAccessProgram> access;shader::ResourceUsage cached_usage;std::map<UniformReadKey,shader::UniformWords> cached_reads;std::set<UniformReadKey> requested_reads;std::vector<std::array<unsigned,3>> state_reads;std::vector<unsigned> state_masks;std::vector<shader::UniformWords> scene_words;std::uint64_t scene_frame{};bool proof_pending{};std::uint64_t retry_after{};};
struct UniformJob {std::shared_ptr<Variant> variant;std::map<UniformReadKey,shader::UniformWords> words;};
struct Pipeline {std::uint64_t id{},profile_id{};Ptr<ID3D12Device> device;std::shared_ptr<Root> root;std::vector<std::byte> code;std::shared_ptr<Variant> variant;std::string reason{"queued"};bool queued{};};
struct Signature {bool known{},compute{};std::shared_ptr<Root> root;std::vector<D3D12_INDIRECT_ARGUMENT_DESC> resets;};
struct Use {std::shared_ptr<Variant> variant;binding::Arguments arguments;std::vector<std::uint64_t> heaps;UINT x{},y{},z{};bool calibration{};std::shared_ptr<SpatialProbeGpu> probe;};
struct CpuRecordingState {
    arc::ExactStateCache cache;std::uint64_t generation{};
    std::atomic<bool> usable{true};std::atomic<std::uint64_t> attempts{},skipped{};
};
struct RawDescriptor {
    std::uint64_t address{},heap{},resource{},counter{};
    ID3D12Resource* native{};ID3D12Resource* native_counter{};
    std::array<std::byte,128> bytes{};std::size_t size{};unsigned kind{};
};
struct Recording {
    std::shared_ptr<CpuRecordingState> cpu_cache;
    ID3D12PipelineState* pipeline{};std::shared_ptr<Root> root;binding::Arguments arguments;
    std::vector<std::uint64_t> heaps;std::shared_ptr<GpuControl> control;std::vector<Use> uses;
    bool valid{true},closed{},epilogue{},render_pass{},arguments_uncertain{};
    bool unpredicated{true};
    unsigned query_depth{};
};
struct SensitivityContext {
    std::uint64_t id{},generation{},last_seen{};unsigned probes{};arc::SpatialSensitivity learner;arc::SpatialModelTable table;std::array<double,256> observed_tiles{};
    SensitivityContext(std::uint64_t key,std::uint64_t epoch):id(key),generation(epoch),learner(key){table=learner.snapshot();}
};
struct PendingProbe {std::shared_ptr<SensitivityContext> context;std::uint64_t frame{},epoch{};bool noise{};};
struct State {
    std::array<std::array<std::uint64_t,4>,64> scene_events{};unsigned scene_event_count{};std::uint64_t scene_events_dropped{};
    bool spatial_learning{},probes_paused{};float spatial_limit{.02f};std::uint64_t sensitivity_generation{1};
    std::map<ID3D12Device*,std::shared_ptr<SpatialProbeGpu>> probe_pools;
    std::map<SpatialProbeGpu*,PendingProbe> pending_probes;
    std::map<std::vector<std::uint64_t>,std::shared_ptr<SensitivityContext>> sensitivity;
    std::map<std::uint64_t,std::uint64_t> last_probe_frames;
    std::uint64_t training_pipeline{};arc::ComputePolicy training_recipe;std::set<std::uint64_t> training_models;
    arc::PolicyBindingEvidence binding_evidence;
    UINT surface_width{},surface_height{},surface_format{};bool center_priority{};std::set<std::uint64_t> presentation_resources;
    std::recursive_mutex mutex,descriptor_mutex;std::condition_variable_any changed;
    std::atomic<bool> enabled{};UINT x_rate{1},y_rate{1},comparison_taps{},zero_factor{},mip_steps{},sample_percent{100};
    std::atomic<bool> cpu_optimize{};std::atomic<std::uint64_t> cpu_generation{1},cpu_lookup_epoch{1};
    std::uint64_t cpu_state_attempts{},cpu_state_skipped{};
    std::array<RawDescriptor,512> cpu_views;
    std::uint64_t cpu_view_skipped{};
    bool heaviest_only{},bundle_mode{};arc::PolicyBundle bundle;
    std::uint64_t calibration_epoch{};
    std::uint64_t calibration_measurement_epoch{},calibration_expected{};
    std::vector<std::uint64_t> catalog_targets,record_targets;
    std::uint64_t selected_pipeline{},cost_session{},cost_prepared{};double selected_cost{};
    bool protect_edges{},instrumentation{true},measure_control{};float edge_threshold{.08f};
    bool compile_on_demand{};
    std::filesystem::path worker,compiler,cache,persistent_cache;
    std::string compiler_identity;
    std::deque<std::string> events;std::uint64_t cache_hits{},cache_misses{},events_dropped{};
    std::unordered_map<ID3D12RootSignature*,std::shared_ptr<Root>> roots;
    std::unordered_map<ID3D12PipelineState*,std::shared_ptr<Pipeline>> pipelines;
    std::unordered_map<ID3D12GraphicsCommandList*,Recording> commands;
    std::unordered_map<ID3D12DescriptorHeap*,binding::DescriptorHeap> heaps;
    std::unordered_map<ID3D12CommandSignature*,Signature> signatures;
    std::map<std::uint64_t,binding::DescriptorHeap> heaps_by_id;
    std::unordered_map<ID3D12Resource*,std::uint64_t> resource_ids;
    std::unordered_map<std::uint64_t,ID3D12Resource*> resource_natives;
    std::map<ID3D12Heap*,std::uint64_t> allocation_heaps;
    std::map<std::uint64_t,binding::Allocation> allocations;
    std::vector<AllocationCache> allocation_cache;
    binding::BufferIndex buffers;
    DescriptorLedger descriptors;
    std::deque<std::shared_ptr<Pipeline>> jobs;
    std::deque<UniformJob> uniform_jobs;
    std::size_t queued_code_bytes{};
    std::vector<std::shared_ptr<GpuControl>> controls;
    std::map<std::uint64_t,VariantStats> history;
    std::uint64_t next{1},prepared{},declined{},modified_dispatches{},coarse_submissions{},neutral_submissions{},faults{},pool_misses{};
    std::string last_error;std::map<std::string,std::uint64_t> admission_reasons;
    std::map<std::string,std::uint64_t> pipeline_declines;
    std::array<std::uint64_t,12> dispatch_declines{};
    std::uint64_t policy_epoch{},last_active_epoch{};
    bool sample_state{};FrameStateSample last_frame_state;
    Ptr<ID3D12CommandQueue> required_queue;
    arc::PolicyBundle learning_bundle;
    std::map<std::vector<std::uint64_t>,std::uint64_t> spatial_keys;
    std::atomic<std::uint64_t> current_frame{},approved_policy{},approved_until{},current_bundle{},approved_submission_frame{};
    std::atomic<std::uint64_t> activity_frames{},activity_approved_frames{},activity_ticks{},activity_approved_ticks{},activity_last_qpc{};
    std::atomic<std::uint64_t> policy_valid_until{};
    bool spatial_capture_requested{};
};
State& state(){static auto* s=new State;return *s;}
void event(nlohmann::json value)noexcept{try{LARGE_INTEGER time{};QueryPerformanceCounter(&time);if(!value.contains("event_qpc"))value["event_qpc"]=time.QuadPart;value["queued_qpc"]=time.QuadPart;auto text=value.dump();auto& s=state();std::lock_guard lock(s.mutex);if(s.events.size()>=256){s.events.pop_front();++s.events_dropped;}s.events.push_back(std::move(text));}catch(...) {}}
RawDescriptor& raw_view(UINT64 address){return state().cpu_views[((address>>5)^(address>>14))%state().cpu_views.size()];}
void forget_raw_view(UINT64 address){auto& old=raw_view(address);if(old.address==address)old.address=0;}
bool view_identity(unsigned kind,ID3D12Resource* native,ID3D12Resource* counter,const void* desc,std::size_t size,D3D12_CPU_DESCRIPTOR_HANDLE handle,RawDescriptor& result){
    auto& s=state();if(!desc||!size||size>result.bytes.size()||!handle.ptr)return false;
    if((kind==1&&size!=sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC))||(kind==2&&size!=sizeof(D3D12_UNORDERED_ACCESS_VIEW_DESC))||(kind==6&&size!=sizeof(D3D12_CONSTANT_BUFFER_VIEW_DESC))||(kind!=1&&kind!=2&&kind!=6))return false;
    if(kind==1&&static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(desc)->ViewDimension==D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)return false;
    result.heap=s.descriptors.heap_at(handle.ptr);if(!result.heap)return false;
    const auto heap=s.heaps_by_id.find(result.heap);if(heap==s.heaps_by_id.end()||heap->second.type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)return false;
    if(native){const auto id=s.resource_ids.find(native);if(id==s.resource_ids.end())return false;result.resource=id->second;}
    if(counter){const auto id=s.resource_ids.find(counter);if(id==s.resource_ids.end())return false;result.counter=id->second;}
    if(kind==6){const auto& value=*static_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(desc);
        if(value.BufferLocation){const auto* allocation=s.buffers.resolve(s.allocations,value.BufferLocation,value.SizeInBytes);if(!allocation)return false;result.resource=allocation->id;}}
    result.address=handle.ptr;result.native=native;result.native_counter=counter;result.kind=kind;result.size=size;return true;
}
void remember_view(unsigned kind,ID3D12Resource* resource,ID3D12Resource* counter,const void* desc,std::size_t size,D3D12_CPU_DESCRIPTOR_HANDLE handle){
    if(!state().cpu_optimize)return;RawDescriptor value;
    if(view_identity(kind,resource,counter,desc,size,handle,value)){std::memcpy(value.bytes.data(),desc,size);raw_view(handle.ptr)=value;}
    else forget_raw_view(handle.ptr);
}
std::atomic<std::uint64_t> cpu_ns{};
std::atomic<std::uint64_t> original_submit_ns{};
std::atomic<bool> cpu_timing{};
struct CpuSample {std::atomic<const char*> name{};std::atomic<std::uint64_t> ns{},calls{},lock_ns{};};
std::array<CpuSample,128> cpu_samples;
std::atomic<unsigned> next_cpu_sample{};
unsigned register_cpu_sample(const char* name){const auto id=next_cpu_sample.fetch_add(1);if(id<cpu_samples.size())cpu_samples[id].name=name;return id;}
thread_local unsigned metering_depth{};
struct CpuMeter {
    using Clock=std::chrono::steady_clock;
    bool outer{metering_depth++==0&&cpu_timing.load(std::memory_order_relaxed)};Clock::time_point start;unsigned sample;
    explicit CpuMeter(unsigned id=UINT_MAX):sample(id){if(outer)start=Clock::now();}
    void locked(){if(outer&&sample<cpu_samples.size())cpu_samples[sample].lock_ns.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count()),std::memory_order_relaxed);}
    ~CpuMeter(){--metering_depth;if(outer){const auto elapsed=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());cpu_ns.fetch_add(elapsed,std::memory_order_relaxed);if(sample<cpu_samples.size()){cpu_samples[sample].ns.fetch_add(elapsed,std::memory_order_relaxed);cpu_samples[sample].calls.fetch_add(1,std::memory_order_relaxed);}}}
};
void check(HRESULT value){if(FAILED(value))throw std::runtime_error("Optimizer HRESULT "+std::to_string(value));}
template<class F>void safe(F&& action,std::source_location source=std::source_location::current())noexcept{static const auto sample=register_cpu_sample(source.function_name());CpuMeter meter(sample);try{std::lock_guard lock(state().mutex);meter.locked();action();}catch(const std::exception& error){std::lock_guard lock(state().mutex);auto& s=state();++s.faults;s.x_rate=s.y_rate=1;s.comparison_taps=0;s.zero_factor=0;s.last_error=error.what();}catch(...){std::lock_guard lock(state().mutex);++state().faults;state().x_rate=state().y_rate=1;state().comparison_taps=0;state().zero_factor=0;state().last_error="unknown exception";}}
constexpr GUID lifetime_guid{0x109dd36a,0xb6f3,0x4f98,{0x89,0x7c,0xf3,0x2c,0xa2,0x60,0xb5,0xb4}};
class Lifetime final:public IUnknown {
    std::atomic<ULONG> count{1};void* object;unsigned kind;
public:
    Lifetime(void* p,unsigned k):object(p),kind(k){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown)return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++count;}
    ULONG STDMETHODCALLTYPE Release()override{arc::InterceptCpuMeter::Scope cpu_hook(!cpu_cost::on_worker_thread());const auto n=--count;if(!n){safe([&]{auto& s=state();
        if(kind==1)s.roots.erase(static_cast<ID3D12RootSignature*>(object));
        else if(kind==2){const auto it=s.pipelines.find(static_cast<ID3D12PipelineState*>(object));if(it!=s.pipelines.end())s.binding_evidence.retire_pipeline(it->second->id);if(it!=s.pipelines.end()&&!it->second->queued)s.queued_code_bytes-=it->second->code.size();s.pipelines.erase(static_cast<ID3D12PipelineState*>(object));++s.cpu_generation;}
        else if(kind==3){auto found=s.commands.find(static_cast<ID3D12GraphicsCommandList*>(object));
            if(found!=s.commands.end()&&found->second.cpu_cache){const auto& cpu=*found->second.cpu_cache;found->second.cpu_cache->usable=false;s.cpu_state_attempts+=cpu.attempts.load();s.cpu_state_skipped+=cpu.skipped.load();}
            ++s.cpu_lookup_epoch;s.commands.erase(static_cast<ID3D12GraphicsCommandList*>(object));}
        else if(kind==4){auto it=s.heaps.find(static_cast<ID3D12DescriptorHeap*>(object));if(it!=s.heaps.end()){s.descriptors.retire_heap(it->second.id);s.heaps_by_id.erase(it->second.id);s.heaps.erase(it);}}
        else if(kind==5){auto it=s.resource_ids.find(static_cast<ID3D12Resource*>(object));if(it!=s.resource_ids.end()){s.binding_evidence.retire(it->second);s.buffers.retire(it->second);s.resource_natives.erase(it->second);s.allocations.erase(it->second);s.resource_ids.erase(it);}}
        else if(kind==6)s.allocation_heaps.erase(static_cast<ID3D12Heap*>(object));
        else if(kind==7)s.signatures.erase(static_cast<ID3D12CommandSignature*>(object));
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
        if(!a->initialized&&!(a->observed&&a->type!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE))continue;
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
shader::UniformWords read_uniform_memory(const Use& use,const std::vector<binding::DescriptorHeap>& heaps,
    std::map<std::pair<unsigned,unsigned>,UniformMemory>& views,unsigned range,unsigned reg,unsigned offset){
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
}
shader::ResourceUsage uniform_usage(const Use& use,const std::vector<binding::DescriptorHeap>& heaps){
    mirror::InternalCall internal;
    std::map<std::pair<unsigned,unsigned>,UniformMemory> views;
    auto read_memory=[&](unsigned range,unsigned reg,unsigned offset){return read_uniform_memory(use,heaps,views,range,reg,offset);};
    auto& variant=*use.variant;
    if(variant.cached_usage.complete){
        bool matches=true;
        // Every cached key is unique. Do not allocate a second memo tree merely
        // to compare it and immediately discard it on the common cache hit.
        for(const auto& [key,words]:variant.cached_reads){const auto [range,reg,offset]=key;if(!(read_memory(range,reg,offset)==words)){matches=false;break;}}
        if(matches){auto result=variant.cached_usage;result.steps=0;return result;}
    }
    // Abstract interpretation can explore many paths. Only bounded CPU-visible
    // snapshots and cached-proof validation belong on the submission thread.
    if(!variant.proof_pending&&variant.uniform_attempts>=variant.retry_after&&state().uniform_jobs.size()<64){
        UniformJob job;job.variant=use.variant;
        for(const auto& key:variant.requested_reads){const auto [range,reg,offset]=key;auto words=read_memory(range,reg,offset);job.words.emplace(key,words);if(words.valid_mask)++variant.uniform_known_reads;else ++variant.uniform_unknown_reads;}
        state().uniform_jobs.push_back(std::move(job));variant.proof_pending=true;state().changed.notify_one();
    }
    shader::ResourceUsage pending;pending.reason=variant.proof_pending?"uniform_proof_pending":"uniform_proof_backoff";return pending;
}

void evaluate_uniform_job(UniformJob job){
    std::map<UniformReadKey,shader::UniformWords> used;bool missing=false,capacity=false;
    struct ReadCapacity {};
    shader::ResourceUsage result;
    try{result=job.variant->access->evaluate([&](unsigned range,unsigned reg,unsigned offset){
        const UniformReadKey key{range,reg,offset};if(const auto prior=used.find(key);prior!=used.end())return prior->second;
        if(used.size()>=256){capacity=true;throw ReadCapacity{};}
        auto found=job.words.find(key);missing|=found==job.words.end();auto value=found==job.words.end()?shader::UniformWords{}:found->second;used.emplace(key,value);return value;
    });}catch(const ReadCapacity&){result.reason="uniform_read_capacity";}
    catch(const std::exception&){result.reason="uniform_worker_failure";}
    std::lock_guard lock(state().mutex);auto& variant=*job.variant;variant.proof_pending=false;variant.uniform_steps+=result.steps;
    variant.requested_reads.clear();for(const auto& [key,words]:used){(void)words;variant.requested_reads.insert(key);}
    if(!missing&&result.complete){
        // No missing input may become a permanent unknown cache entry. Every
        // recorded word (including validity bits) is checked on the next submit.
        result.steps=0;variant.cached_usage=std::move(result);variant.cached_reads=std::move(used);variant.retry_after=0;
    }else{
        variant.cached_usage={};variant.cached_reads.clear();
        if((capacity&&!missing)||(!missing&&!result.complete))variant.retry_after=variant.uniform_attempts+120;
    }
}
std::shared_ptr<Variant> prepare_variant(const std::shared_ptr<Pipeline>& pipeline){
    auto& s=state();const auto source=s.cache/(std::to_string(pipeline->id)+".source.bin"),binary=s.cache/(std::to_string(pipeline->id)+".controlled.bin");
    struct ScratchCleanup {std::filesystem::path source,binary;bool retain{};~ScratchCleanup(){if(retain)return;std::error_code ignored;std::filesystem::remove(source,ignored);for(const wchar_t* ending:{L"",L".contract",L".access.ll",L".spatial.bin",L".probe.bin",L".worker.txt"}){auto path=binary;path+=ending;std::filesystem::remove(path,ignored);}}};
    ScratchCleanup cleanup{source,binary,environment(L"ARC_CAPTURE_SHADER_CODE")==L"1"};
    std::set<UINT> spaces;for(const auto& p:pipeline->root->layout->parameters){if(p.type==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){for(const auto& r:p.ranges)spaces.insert(r.space);}else spaces.insert(p.space);}for(const auto& sampler:pipeline->root->layout->samplers)spaces.insert(sampler.RegisterSpace);
    UINT space=0;while(spaces.contains(space)&&space<65536)++space;if(space==65536)throw std::runtime_error("control register space capacity");
    if(s.compiler_identity.empty())s.compiler_identity=shader_cache::file_digest(s.worker)+shader_cache::file_digest(s.compiler);
    const auto identity=std::string("ARC_CONTROLLED_EXECUTION_1:")+shader_cache::digest(pipeline->code)+shader_cache::digest(pipeline->root->bytes)+s.compiler_identity+":"+std::to_string(space);
    const auto key=shader_cache::digest({reinterpret_cast<const std::byte*>(identity.data()),identity.size()});
    event({{"phase","shader_identity"},{"pipeline",pipeline->id},{"pipeline_generation",pipeline->id},{"profile_identity",pipeline->profile_id},{"shader_sha256",shader_cache::digest(pipeline->code)},{"layout_sha256",shader_cache::digest(pipeline->root->bytes)},{"analysis_key",key}});
    if(const auto declined=shader_cache::restore_decline(s.persistent_cache,key);!declined.empty()){
        event({{"phase","analysis_cache_declined"},{"pipeline",pipeline->id},{"shader_key",key},{"reason",declined}});throw std::runtime_error(declined);
    }
    const bool cached=shader_cache::restore(s.persistent_cache,key,binary);
    {std::lock_guard lock(s.mutex);if(cached)++s.cache_hits;else ++s.cache_misses;}
    event({{"phase",cached?"analysis_cache_hit":"analysis_cache_miss"},{"pipeline",pipeline->id},{"shader_key",key}});
    if(!cached){
    while(background_quality_waiters().load())Sleep(2);
    std::lock_guard background_gate(background_compute_gate());
    for(const wchar_t* suffix:{L"",L".contract",L".access.ll",L".spatial.bin",L".probe.bin"}){auto partial=binary;partial+=suffix;std::error_code ignored;std::filesystem::remove(partial,ignored);}
    {std::ofstream file(source,std::ios::binary);file.write(reinterpret_cast<const char*>(pipeline->code.data()),pipeline->code.size());file.close();if(!file)throw std::runtime_error("shader source cache IO");}
    auto command=quote(s.worker.wstring())+L" controlled-proof:"+std::to_wstring(space)+L" "+quote(source.wstring())+L" "+quote(binary.wstring())+L" "+quote(s.compiler.wstring());
    auto log_path=binary;log_path+=L".worker.txt";SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE log=CreateFileW(log_path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE input_handle=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;start.wShowWindow=SW_HIDE;start.hStdOutput=start.hStdError=log;start.hStdInput=input_handle;PROCESS_INFORMATION process{};
    const bool launched=log!=INVALID_HANDLE_VALUE&&input_handle!=INVALID_HANDLE_VALUE&&CreateProcessW(s.worker.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|BELOW_NORMAL_PRIORITY_CLASS,nullptr,s.cache.c_str(),&start,&process);
    if(log!=INVALID_HANDLE_VALUE)CloseHandle(log);if(input_handle!=INVALID_HANDLE_VALUE)CloseHandle(input_handle);if(!launched)throw std::runtime_error("shader worker launch");
    struct ChildJob {HANDLE handle{CreateJobObjectW(nullptr,nullptr)};~ChildJob(){if(handle)CloseHandle(handle);}} child_job;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION child_limits{};child_limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!child_job.handle||!SetInformationJobObject(child_job.handle,JobObjectExtendedLimitInformation,&child_limits,sizeof(child_limits))||!AssignProcessToJobObject(child_job.handle,process.hProcess)){TerminateProcess(process.hProcess,2);WaitForSingleObject(process.hProcess,1000);CloseHandle(process.hThread);CloseHandle(process.hProcess);throw std::runtime_error("shader worker job ownership");}
    cpu_cost::Registration child_cpu(cpu_cost::Kind::Compiler,process.hProcess,process.hThread);
    if(ResumeThread(process.hThread)==DWORD(-1)){TerminateProcess(process.hProcess,2);WaitForSingleObject(process.hProcess,1000);CloseHandle(process.hThread);CloseHandle(process.hProcess);throw std::runtime_error("shader worker resume");}
    CloseHandle(process.hThread);DWORD waited=WAIT_TIMEOUT;const auto worker_deadline=GetTickCount64()+20000;
    while(GetTickCount64()<worker_deadline&&(waited=WaitForSingleObject(process.hProcess,50))==WAIT_TIMEOUT)cpu_cost::memory_snapshot();
    DWORD code=1;
    if(waited!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,2);WaitForSingleObject(process.hProcess,1000);}else GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);
    if(code){std::ifstream log(log_path);std::string reason;std::getline(log,reason);reason=reason.substr(0,512);
        if(reason.starts_with("Shader declined:")){shader_cache::store_decline(s.persistent_cache,key,reason);shader_cache::trim(s.persistent_cache);}
        throw std::runtime_error(reason.empty()?"shader_worker_failed":reason);}
    }
    auto manifest=binary;manifest+=L".contract";std::ifstream description(manifest);std::string tag;description>>tag;
    auto result=std::make_shared<Variant>();result->id=pipeline->id;result->analysis_key=key;auto& contract=result->contract;std::size_t count{};
    description>>contract.control_space>>contract.threads[0]>>contract.threads[1]>>contract.threads[2]>>contract.stores>>count>>contract.comparison_filter_groups>>contract.zero_factor_regions>>contract.edge_input_mask>>contract.mip_samples;
    description>>contract.execution_marker>>contract.probe_outputs>>contract.group_shared>>contract.sample_loops;
    if(!description||tag!="ARC_SHADER_CONTRACT_12"||!contract.execution_marker||count>128||contract.control_space==UINT32_MAX)throw std::runtime_error("shader worker contract");
    for(std::size_t i=0;i<count;++i){shader::ResourceContract r;description>>r.resource_class>>r.range_id>>r.shader_register>>r.space>>r.count>>r.kind;contract.resources.push_back(r);}
    if(!description)throw std::runtime_error("truncated shader contract");contract.admitted=true;
    std::string extension;if(description>>extension){if(extension!="ray_loops"||!(description>>contract.ray_loops)||contract.ray_loops>contract.sample_loops)throw std::runtime_error("ray loop contract");}
    auto access_path=binary;access_path+=L".access.ll";
    if(std::filesystem::is_regular_file(access_path)&&std::filesystem::file_size(access_path)<=8*1024*1024){std::ifstream access(access_path,std::ios::binary);std::string ir{std::istreambuf_iterator<char>(access),{}};result->access=shader::UniformAccessProgram::compile(ir);for(const auto& read:shader::floating_uniform_components(ir)){result->state_reads.push_back({read[0],read[1],read[2]});result->state_masks.push_back(read[3]);}result->scene_words.resize(result->state_reads.size());}
    const auto root=binding::append_control_cbv(pipeline->root->bytes,contract.control_space,contract.execution_marker);if(root.empty())throw std::runtime_error("root_cannot_add_control");
    const auto size=std::filesystem::file_size(binary);if(!size||size>8*1024*1024)throw std::runtime_error("shader worker output size");
    std::ifstream input(binary,std::ios::binary);std::vector<char> bytes{std::istreambuf_iterator<char>(input),{}};
    if(bytes.size()!=size)throw std::runtime_error("shader worker output IO");
    mirror::InternalCall internal;check(pipeline->device->CreateRootSignature(0,root.data(),root.size(),IID_PPV_ARGS(&result->root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=result->root.Get();p.CS={bytes.data(),bytes.size()};check(pipeline->device->CreateComputePipelineState(&p,IID_PPV_ARGS(&result->pipeline)));
    if(contract.edge_input_mask){auto spatial=binary;spatial+=L".spatial.bin";const auto size=std::filesystem::file_size(spatial);if(!size||size>1024*1024)throw std::runtime_error("Spatial prepass size");std::ifstream file(spatial,std::ios::binary);std::vector<char> code{std::istreambuf_iterator<char>(file),{}};if(code.size()!=size)throw std::runtime_error("Spatial prepass read");p.CS={code.data(),code.size()};check(pipeline->device->CreateComputePipelineState(&p,IID_PPV_ARGS(&result->spatial_pipeline)));}
    if(contract.probe_outputs){
        const auto bytes=binding::append_control_cbv(pipeline->root->bytes,contract.control_space,true,true);
        if(!bytes.empty()){check(pipeline->device->CreateRootSignature(0,bytes.data(),bytes.size(),IID_PPV_ARGS(&result->probe_root)));auto path=binary;path+=L".probe.bin";const auto size=std::filesystem::file_size(path);if(!size||size>8*1024*1024)throw std::runtime_error("Probe variant size");std::ifstream file(path,std::ios::binary);std::vector<char> code{std::istreambuf_iterator<char>(file),{}};if(code.size()!=size)throw std::runtime_error("Probe variant read");p.pRootSignature=result->probe_root.Get();p.CS={code.data(),code.size()};check(pipeline->device->CreateComputePipelineState(&p,IID_PPV_ARGS(&result->probe_pipeline)));}
    }
    if(!cached){const bool stored=shader_cache::store(s.persistent_cache,key,binary);event({{"phase","analysis_cache_store"},{"pipeline",pipeline->id},{"shader_key",key},{"stored",stored}});shader_cache::trim(s.persistent_cache);}
    event({{"phase","analysis_ready"},{"pipeline",pipeline->id},{"shader_key",key},{"coarse",true},{"comparison_groups",contract.comparison_filter_groups},{"sample_loops",contract.sample_loops},{"ray_loops",contract.ray_loops},{"group_shared",contract.group_shared},{"mip_samples",contract.mip_samples},{"edge_inputs",contract.edge_input_mask}});
    return result;
}
DWORD WINAPI worker(void*){
    cpu_cost::Registration worker_cpu;
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    for(;;){std::shared_ptr<Pipeline> job;UniformJob proof;{
        auto& s=state();std::unique_lock lock(s.mutex);s.changed.wait(lock,[&]{return !s.jobs.empty()||!s.uniform_jobs.empty();});
        if(!s.uniform_jobs.empty()){proof=std::move(s.uniform_jobs.front());s.uniform_jobs.pop_front();}
        else{job=std::move(s.jobs.front());s.jobs.pop_front();}}
        if(proof.variant){auto variant=proof.variant;try{evaluate_uniform_job(std::move(proof));}catch(...){safe([&]{variant->proof_pending=false;variant->retry_after=variant->uniform_attempts+120;variant->cached_usage={};variant->cached_reads.clear();++state().faults;});}continue;}
        try{
            auto variant=prepare_variant(job);std::vector<std::shared_ptr<GpuControl>> controls;std::shared_ptr<SpatialProbeGpu> probe_pool;
            bool needs_pool=false;{std::lock_guard lock(state().mutex);needs_pool=std::none_of(state().controls.begin(),state().controls.end(),[&](const auto& c){return c->device()==job->device.Get();});}
            if(needs_pool){if(state().controls.size()+8>8)throw std::runtime_error("spatial_control_budget_256MiB_multiple_device_declined");mirror::InternalCall internal;for(unsigned i=0;i<8;++i)controls.push_back(std::make_shared<GpuControl>(job->device.Get(),state().measure_control,true));probe_pool=std::make_shared<SpatialProbeGpu>(job->device.Get(),8ull*1024*1024);}
            std::lock_guard lock(state().mutex);job->variant=std::move(variant);job->reason="prepared_neutral";state().queued_code_bytes-=job->code.size();job->code.clear();job->code.shrink_to_fit();++state().prepared;state().controls.insert(state().controls.end(),controls.begin(),controls.end());if(probe_pool)state().probe_pools[job->device.Get()]=std::move(probe_pool);
        }catch(const std::exception& error){event({{"phase","analysis_declined"},{"pipeline",job->id},{"reason",error.what()}});std::lock_guard lock(state().mutex);job->reason=error.what();state().queued_code_bytes-=job->code.size();job->code.clear();job->code.shrink_to_fit();++state().declined;}
    }
}
}
bool enabled()noexcept{return state().enabled.load(std::memory_order_relaxed);}
std::vector<std::string> drain_events()noexcept{std::vector<std::string> result;try{auto& s=state();std::lock_guard lock(s.mutex);result.reserve(s.events.size());while(!s.events.empty()){result.push_back(std::move(s.events.front()));s.events.pop_front();}}catch(...){}return result;}
namespace {
std::pair<std::filesystem::path,std::string> performance_location(std::uint64_t pipeline,const std::string& profile){
    Ptr<ID3D12Device> device;std::string analysis;std::filesystem::path root;UINT width{},height{},format{};
    {auto& s=state();std::lock_guard lock(s.mutex);for(const auto& [native,p]:s.pipelines)if(p->id==pipeline&&p->variant){device=p->device;analysis=p->variant->analysis_key;break;}root=s.persistent_cache/L"performance-proposals";width=s.surface_width;height=s.surface_height;format=s.surface_format;}
    if(!device||analysis.empty()||!width||!height||(profile!="balanced"&&profile!="aggressive"))return {};
    Ptr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Ptr<IDXGIAdapter1> adapter;check(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)));DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc));LARGE_INTEGER driver{};check(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver));
    std::ostringstream identity;identity<<"ARC_PERFORMANCE_PROPOSAL_1:"<<analysis<<':'<<desc.VendorId<<':'<<desc.DeviceId<<':'<<desc.SubSysId<<':'<<desc.Revision<<':'<<driver.QuadPart<<':'<<width<<':'<<height<<':'<<format<<':'<<profile;
    return {root,proposal::hash(identity.str())};
}
}
std::optional<arc::ComputePolicy> performance_proposal(std::uint64_t pipeline,const std::string& profile)noexcept{try{const auto [root,key]=performance_location(pipeline,profile);auto result=proposal::load(root,key,pipeline);event({{"phase","performance_proposal_cache"},{"pipeline",pipeline},{"key",key},{"hit",bool(result)},{"requires_fresh_validation",true}});return result;}catch(...){return {};}}
void remember_performance_proposal(const arc::ComputePolicy& policy,const std::string& profile)noexcept{try{const auto [root,key]=performance_location(policy.pipeline,profile);const bool saved=proposal::save(root,key,policy);event({{"phase","performance_proposal_saved"},{"pipeline",policy.pipeline},{"key",key},{"saved",saved},{"requires_fresh_validation",true}});}catch(...){}}
void spatial_learning(bool enabled,float limit)noexcept{safe([&]{auto& s=state();if(s.spatial_learning!=enabled||s.spatial_limit!=limit){++s.sensitivity_generation;s.sensitivity.clear();s.training_models.clear();}s.spatial_learning=enabled;s.spatial_limit=std::clamp(limit,0.f,.08f);});}
void pause_spatial_probes(bool paused)noexcept{safe([&]{state().probes_paused=paused;});}
bool begin_spatial_training(const arc::PolicyBundle& bundle,std::uint64_t pipeline)noexcept{bool result=false;safe([&]{auto& s=state();const auto* recipe=bundle.find(pipeline);if(!recipe||!recipe->protect_edges||!s.spatial_learning)return;s.training_pipeline=pipeline;s.training_recipe=*recipe;s.training_models.clear();s.probes_paused=false;result=true;});return result;}
void end_spatial_training()noexcept{safe([&]{state().training_pipeline=0;state().training_models.clear();});}
SpatialProgress spatial_progress()noexcept{SpatialProgress result;safe([&]{auto& s=state();double allowed=0,total=0;for(const auto& [key,model]:s.sensitivity){if(!s.training_models.contains(model->id))continue;++result.models;result.probes+=model->probes;bool sampled=false,eligible=false;for(unsigned bin=0;bin<256;++bin){const auto& e=model->table.entries[bin];const bool fresh=s.current_frame>=e.last_frame&&s.current_frame-e.last_frame<=600;sampled|=e.samples>=3&&fresh;const bool pass=e.samples>=3&&fresh&&!e.invalid&&e.error<=s.spatial_limit*std::clamp(s.training_recipe.edge_threshold,0.f,1.f);eligible|=pass;total+=model->observed_tiles[bin];if(pass)allowed+=model->observed_tiles[bin];result.best_error=std::min(result.best_error,e.error);}result.sampled+=sampled;result.eligible+=eligible;}result.eligible_fraction=total?allowed/total:0;});return result;}
void request_spatial_diagnostics()noexcept{safe([&]{state().spatial_capture_requested=true;});}
std::array<std::uint64_t,3> control_allocation_bytes()noexcept{std::array<std::uint64_t,3> result{};safe([&]{for(const auto& control:state().controls){const auto bytes=control->allocation_bytes();for(unsigned i=0;i<3;++i)result[i]+=bytes[i];}for(const auto& [device,pool]:state().probe_pools){const auto bytes=pool->allocation_bytes();for(unsigned i=0;i<3;++i)result[i]+=bytes[i];}});return result;}
std::string spatial_snapshot()noexcept{
    try{std::vector<GpuControl::SpatialReadback> candidates;std::uint64_t selected{};{std::lock_guard lock(state().mutex);selected=state().selected_pipeline;for(const auto& control:state().controls)if(auto map=control->spatial_readback())candidates.push_back(std::move(*map));}
        std::sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b){const bool ap=a.pipeline==selected,bp=b.pipeline==selected;return ap!=bp?ap:a.frame>b.frame;});
        for(const auto& capture:candidates){std::vector<GpuControl::SpatialTile> tiles;if(!capture.read(tiles))continue;
            nlohmann::json values=nlohmann::json::array();unsigned unknown=0,coarse=0;
            for(const auto& tile:tiles){const bool valid=tile.key==capture.key&&tile.frame==capture.frame&&tile.mode<=15&&std::isfinite(tile.importance)&&std::isfinite(tile.confidence)&&tile.confidence>=0&&tile.confidence<=1;
                const bool confident=valid&&tile.confidence>0;unknown+=!confident;coarse+=confident&&tile.mode!=0;
                values.push_back({valid?tile.importance:1000.f,confident?tile.mode:0u,confident?tile.confidence:0.f});}
            return nlohmann::json{{"schema",1},{"coordinate_space",capture.screen_coordinates?"backbuffer_texel":"pass_texel"},{"final_screen_correspondence",capture.screen_coordinates},{"center_priority",capture.center_priority},{"quality_certificate",false},{"pipeline",capture.pipeline},{"binding_key",capture.key},{"frame",capture.frame},{"queue",capture.queue},{"width",capture.width},{"height",capture.height},{"tile_width",capture.tile_width},{"tile_height",capture.tile_height},{"tiles_x",(capture.width+capture.tile_width-1)/capture.tile_width},{"tiles_y",(capture.height+capture.tile_height-1)/capture.tile_height},{"value_kind",capture.learned?"measured_transform_error_bound":"input_detail_heuristic"},{"rate",{capture.x_rate,capture.y_rate}},{"mip_steps",capture.mip_steps},{"sample_percent",capture.sample_percent},{"policy_budget_fraction",capture.threshold},{"edge_threshold",capture.learned?capture.local_error_limit:capture.threshold},{"unknown_tiles",unknown},{"coarse_tiles",coarse},{"layout","importance_mode_confidence"},{"tiles",std::move(values)}}.dump();
        }
    }catch(...){}return {};
}
bool cpu_enabled()noexcept{auto& s=state();const auto until=s.policy_valid_until.load(std::memory_order_relaxed);return s.cpu_optimize.load(std::memory_order_relaxed)&&(!until||s.current_frame.load(std::memory_order_relaxed)<until);}
bool cpu_configure(bool enabled)noexcept{
    if(!optimizer::enabled())return false;
    bool ok=false;safe([&]{auto& s=state();if(s.faults&&enabled)return;++s.cpu_generation;for(auto& view:s.cpu_views)view.address=0;s.cpu_optimize=enabled;ok=true;});return ok;
}
bool cpu_state(ID3D12GraphicsCommandList* native,unsigned slot,const void* bytes,std::size_t size)noexcept{
    if(!cpu_enabled())return false;auto& s=state();
    struct Hint {ID3D12GraphicsCommandList* native{};std::uint64_t epoch{};std::shared_ptr<CpuRecordingState> value;};
    thread_local std::array<Hint,8> hints;
    const auto address=reinterpret_cast<std::uintptr_t>(native);auto& hint=hints[((address>>4)^(address>>12))%hints.size()];
    if(hint.native!=native||hint.epoch!=s.cpu_lookup_epoch.load(std::memory_order_acquire)){
        hint={};safe([&]{auto* c=recording(native);if(!c||!c->valid||c->closed)return;
            if(!c->cpu_cache)c->cpu_cache=std::make_shared<CpuRecordingState>();
            hint={native,s.cpu_lookup_epoch.load(),c->cpu_cache};
        });
    }
    const auto& data=hint.value;if(!data||!data->usable.load(std::memory_order_acquire))return false;
    // DX12 command recording is serialized by the application. A shared cache
    // follows a command when that recording moves between application threads;
    // only its lifetime lookup is thread-local, never its native state.
    const auto generation=s.cpu_generation.load(std::memory_order_acquire);
    if(data->generation!=generation){data->cache.invalidate();data->generation=generation;}
    data->attempts.fetch_add(1,std::memory_order_relaxed);
    const bool skip=data->cache.repeat(slot,bytes,size);if(skip)data->skipped.fetch_add(1,std::memory_order_relaxed);return skip;
}
void cpu_invalidate(ID3D12GraphicsCommandList* native)noexcept{if(cpu_enabled())safe([&]{if(auto* c=recording(native);c&&c->cpu_cache)c->cpu_cache->cache.invalidate();});}
void cpu_objects_changed()noexcept{if(cpu_enabled())safe([&]{++state().cpu_generation;});}
void cpu_cache_snapshot(std::ostream& out){std::lock_guard lock(state().mutex);const auto& s=state();auto attempts=s.cpu_state_attempts,skipped=s.cpu_state_skipped;for(const auto& [native,c]:s.commands)if(c.cpu_cache){attempts+=c.cpu_cache->attempts.load();skipped+=c.cpu_cache->skipped.load();}out<<"{\"enabled\":"<<(s.cpu_optimize?"true":"false")<<",\"attempts\":"<<attempts<<",\"skipped_native_setters\":"<<skipped<<",\"skipped_descriptor_creations\":"<<s.cpu_view_skipped<<'}';}
CpuCacheCounters cpu_cache_counters()noexcept{CpuCacheCounters result;safe([&]{const auto& s=state();result.skipped=s.cpu_state_skipped+s.cpu_view_skipped;result.controlled_submissions=s.coarse_submissions+s.neutral_submissions;for(const auto& [native,c]:s.commands)if(c.cpu_cache)result.skipped+=c.cpu_cache->skipped.load();});return result;}
bool cpu_same_view(unsigned kind,ID3D12Resource* resource,ID3D12Resource* counter,const void* desc,std::size_t size,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{
    if(!cpu_enabled()||!desc||size>128)return false;bool same=false;
    safe([&]{auto& s=state();const auto& old=raw_view(handle.ptr);
        if(!s.cpu_optimize||old.address!=handle.ptr||old.kind!=kind||old.size!=size||old.native!=resource||old.native_counter!=counter||std::memcmp(old.bytes.data(),desc,size))return;
        RawDescriptor current;if(!view_identity(kind,resource,counter,desc,size,handle,current))return;
        same=old.heap==current.heap&&old.resource==current.resource&&old.counter==current.counter;s.cpu_view_skipped+=same;
    });return same;
}
void forget_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{if(enabled())safe([&]{forget_raw_view(handle.ptr);state().descriptors.forget(handle.ptr);});}
PolicyStamp policy_stamp()noexcept{PolicyStamp stamp;safe([&]{const auto& s=state();stamp={s.policy_epoch,s.coarse_submissions,s.last_active_epoch,s.selected_pipeline};});return stamp;}
void sample_frame_state(bool enabled)noexcept{safe([&]{state().sample_state=enabled;state().last_frame_state={};});}
FrameStateSample frame_state_sample(){std::lock_guard lock(state().mutex);return state().last_frame_state;}
std::vector<GpuControl::ExecutionReadback> capture_execution(ID3D12CommandQueue* queue)noexcept{
    std::vector<GpuControl::ExecutionReadback> result;safe([&]{for(const auto& control:state().controls)if(auto proof=control->execution_readback(queue))result.push_back(std::move(*proof));});return result;
}
std::uint64_t binding_evidence_revision()noexcept{std::uint64_t result{};safe([&]{result=state().binding_evidence.revision();});return result;}
bool seal_binding_evidence(std::uint64_t policy)noexcept{bool result=false;safe([&]{result=state().binding_evidence.seal(policy);});return result;}
void reset_binding_evidence()noexcept{safe([&]{state().binding_evidence.clear();state().approved_policy=0;++state().sensitivity_generation;state().sensitivity.clear();state().training_models.clear();});}
void center_priority(bool enabled)noexcept{safe([&]{state().center_priority=enabled;});}
void presentation_surface(IDXGISwapChain* swap)noexcept{try{
    std::vector<Ptr<ID3D12Resource>> buffers;DXGI_SWAP_CHAIN_DESC desc{};
    if(swap&&SUCCEEDED(swap->GetDesc(&desc))&&desc.BufferCount<=16){mirror::InternalCall internal;for(UINT i=0;i<desc.BufferCount;++i){Ptr<ID3D12Resource> buffer;if(SUCCEEDED(swap->GetBuffer(i,IID_PPV_ARGS(&buffer))))buffers.push_back(std::move(buffer));}}
    safe([&]{auto& s=state();s.surface_width=desc.BufferDesc.Width;s.surface_height=desc.BufferDesc.Height;s.surface_format=desc.BufferDesc.Format;s.presentation_resources.clear();for(const auto& buffer:buffers)if(const auto id=resource_identity(buffer.Get()))s.presentation_resources.insert(id);});
}catch(...){safe([&]{state().presentation_resources.clear();});}
}
void require_presentation_queue(ID3D12CommandQueue* queue)noexcept{Ptr<ID3D12CommandQueue> next=queue;safe([&]{state().required_queue.Swap(next);});}
void present_frame(std::uint64_t frame,std::uint64_t qpc)noexcept{auto& s=state();const auto previous=s.current_frame.exchange(frame,std::memory_order_relaxed);const auto approved=s.approved_policy.load(std::memory_order_relaxed);const bool active=approved&&previous<s.approved_until.load(std::memory_order_relaxed)&&(s.approved_submission_frame.load(std::memory_order_relaxed)==previous||(s.cpu_optimize.load(std::memory_order_relaxed)&&s.current_bundle.load(std::memory_order_relaxed)==approved));
    ++s.activity_frames;if(active)++s.activity_approved_frames;if(qpc){const auto last=s.activity_last_qpc.exchange(qpc);if(last&&qpc>=last){s.activity_ticks+=qpc-last;if(active)s.activity_approved_ticks+=qpc-last;}}}
void reset_activity_counters()noexcept{auto& s=state();s.activity_frames=s.activity_approved_frames=s.activity_ticks=s.activity_approved_ticks=s.activity_last_qpc=0;s.approved_policy=0;}
std::uint64_t active_approved_policy()noexcept{auto& s=state();const auto id=s.approved_policy.load();return id&&s.current_bundle.load()==id&&s.current_frame.load()<s.approved_until.load()?id:0;}
void approve_policy(std::uint64_t policy,std::uint64_t until)noexcept{auto& s=state();s.approved_until=until;s.approved_policy=policy;}
std::array<std::uint64_t,4> activity_counters()noexcept{auto& s=state();return {s.activity_frames.load(),s.activity_approved_frames.load(),s.activity_ticks.load(),s.activity_approved_ticks.load()};}
bool restoration_ready()noexcept{bool ready=false;safe([&]{const auto& s=state();if(s.faults||!s.bundle.compute.empty()||s.x_rate!=1||s.y_rate!=1||s.comparison_taps||s.zero_factor||s.mip_steps)return;
    ready=std::all_of(s.controls.begin(),s.controls.end(),[](const auto& control){return SUCCEEDED(control->device()->GetDeviceRemovedReason())&&control->ready();});});return ready;}
ConnectionCoverage connection_coverage()noexcept{ConnectionCoverage result;safe([&]{const auto& s=state();result={s.roots.size(),s.pipelines.size(),s.dispatch_declines[4]};});return result;}
void coverage_snapshot(std::ostream& out){std::lock_guard lock(state().mutex);out<<"{\"observed_root_signatures\":"<<state().roots.size()<<",\"observed_compute_pipelines\":"<<state().pipelines.size()<<",\"pipeline_declines\":{";bool first=true;for(const auto& [reason,count]:state().pipeline_declines){if(!first)out<<',';first=false;out<<std::quoted(reason)<<':'<<count;}
    out<<"},\"dispatch_declines\":{";const char* reasons[]{"recording_unknown","recording_invalid","recording_closed","inside_render_pass","root_unknown","use_capacity","pipeline_unknown","variant_unavailable","root_mismatch","policy_off","not_selected","root_arguments_uncertain"};for(unsigned i=0;i<state().dispatch_declines.size();++i){if(i)out<<',';out<<std::quoted(reasons[i])<<':'<<state().dispatch_declines[i];}
    out<<"},\"pipelines\":[";first=true;for(const auto& [native,p]:state().pipelines){(void)native;if(!first)out<<',';first=false;out<<"{\"id\":"<<p->id<<",\"profile_id\":"<<p->profile_id<<",\"ready\":"<<(p->variant?"true":"false")<<",\"reason\":"<<std::quoted(p->reason)<<'}';}out<<"]}";}
std::uint64_t cpu_nanoseconds()noexcept{return cpu_ns.load(std::memory_order_relaxed);}
void intercept_cpu_snapshot(std::ostream& out){const auto c=arc::InterceptCpuMeter::snapshot();const auto workers=cpu_cost::snapshot();
    out<<"{\"enabled\":"<<(arc::InterceptCpuMeter::enabled()?"true":"false")<<",\"measurement\":\"all_interceptor_wall_excluding_application_native_calls\",\"own_ms\":"<<c.own_ns/1.e6<<",\"excluded_native_ms\":"<<c.excluded_native_ns/1.e6<<",\"calls\":"<<c.calls<<",\"worker_thread_cpu_ms\":"<<workers.nanoseconds[0]/1.e6<<",\"compiler_cpu_ms\":"<<workers.nanoseconds[1]/1.e6<<",\"critic_cpu_ms\":"<<workers.nanoseconds[2]/1.e6<<",\"worker_measurement_failures\":"<<workers.failures<<",\"live_workers\":["<<workers.live[0]<<','<<workers.live[1]<<','<<workers.live[2]<<"],\"complete_overhead_evidence\":false,\"sites\":[";bool first=true;for(const auto& site:arc::InterceptCpuMeter::sites()){const auto name=site.name.load();const auto calls=site.calls.load();if(!name||!calls)continue;if(!first)out<<',';first=false;out<<"{\"name\":"<<std::quoted(name)<<",\"calls\":"<<calls<<",\"own_ms\":"<<site.own_ns.load()/1.e6<<'}';}out<<"]}";
}
CandidateCapabilities candidate_capabilities()noexcept{CandidateCapabilities result;safe([&]{const auto& s=state();if(s.faults||!s.selected_pipeline)return;
    for(const auto& [native,p]:s.pipelines){(void)native;if(p->id!=s.selected_pipeline||!p->variant)continue;const auto& c=p->variant->contract;
        result={p->id,true,c.comparison_filter_groups!=0,c.zero_factor_regions!=0,c.edge_input_mask!=0&&(!s.spatial_learning||p->variant->probe_pipeline),c.mip_samples!=0,c.sample_loops!=0};break;}
});return result;}
std::vector<WorkCandidate> candidate_catalog()noexcept{
    // Profiling has a separate lock; never acquire it under the submission lock.
    auto costs=gpu_profile::compute_costs();std::vector<WorkCandidate> result;
    std::vector<nlohmann::json> measured;static std::map<std::uint64_t,std::pair<std::uint64_t,std::string>> published;
    std::sort(costs.begin(),costs.end(),[](const auto& a,const auto& b){return a.total_gpu_ms>b.total_gpu_ms;});
    safe([&]{auto& s=state();if(s.faults)return;
        for(const auto& cost:costs){const auto found=s.pipelines.find(cost.pipeline);if(found==s.pipelines.end()||found->second->profile_id!=cost.pipeline_identity||!cost.present_windows)continue;
            const auto& p=*found->second;const auto tag=std::make_pair(cost.session,p.reason);if(published.contains(p.id)&&published[p.id]==tag)continue;if(published.size()>=16384)published.clear();published[p.id]=tag;
            measured.push_back({{"phase","pipeline_cost"},{"pipeline",p.id},{"profile_identity",p.profile_id},{"profile_session",cost.session},{"gpu_ms_per_frame",cost.total_gpu_ms/cost.present_windows},{"present_windows",cost.present_windows},{"prepared",bool(p.variant)},{"reason",p.reason}});
        }
        if(s.compile_on_demand){unsigned scheduled=0;
            for(const auto& cost:costs){const auto found=s.pipelines.find(cost.pipeline);if(found==s.pipelines.end()||found->second->profile_id!=cost.pipeline_identity)continue;auto& p=*found->second;
                if(p.variant){if(++scheduled==arc::PolicyBundle::capacity)break;continue;}
                if(p.code.empty())continue;
                if(!p.queued&&s.jobs.size()<64){s.jobs.push_back(found->second);p.queued=true;p.reason="queued_by_measured_cost";s.changed.notify_one();}
                if(++scheduled==arc::PolicyBundle::capacity)break;
            }
        }
        for(const auto& cost:costs){
            const auto p=s.pipelines.find(cost.pipeline);
            if(p==s.pipelines.end()||!p->second->variant||p->second->profile_id!=cost.pipeline_identity||!cost.present_windows||!std::isfinite(cost.total_gpu_ms)||cost.total_gpu_ms<=0)continue;
            const auto& c=p->second->variant->contract;
            result.push_back({{p->second->id,true,c.comparison_filter_groups!=0,c.zero_factor_regions!=0,c.edge_input_mask!=0&&(!s.spatial_learning||p->second->variant->probe_pipeline),c.mip_samples!=0,c.sample_loops!=0},cost.total_gpu_ms/cost.present_windows,cost.session});
        }
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.gpu_ms_per_window!=b.gpu_ms_per_window?a.gpu_ms_per_window>b.gpu_ms_per_window:a.capabilities.pipeline<b.capabilities.pipeline;});
        if(result.size()>arc::PolicyBundle::capacity)result.resize(arc::PolicyBundle::capacity);
        s.catalog_targets.clear();for(const auto& c:result)s.catalog_targets.push_back(c.capabilities.pipeline);
    });for(auto& row:measured)event(std::move(row));return result;
}
bool configure_bundle(const arc::PolicyBundle& bundle,bool apply,std::uint64_t calibration_epoch,std::uint64_t valid_until_frame)noexcept{
    if(!enabled()||!bundle.valid())return false;
    bool accepted=false;safe([&]{auto& s=state();if(s.faults||(calibration_epoch&&(!apply||!s.measure_control||bundle.compute.empty())))return;
        for(const auto& setting:bundle.compute){
            const auto p=std::find_if(s.pipelines.begin(),s.pipelines.end(),[&](const auto& pair){return pair.second->id==setting.pipeline;});
            if(p==s.pipelines.end()||!p->second->variant)return;
            const auto& c=p->second->variant->contract;
            if((setting.comparison_taps&&!c.comparison_filter_groups)||(setting.zero_factor&&!c.zero_factor_regions)||(setting.sample_percent!=100&&!c.sample_loops)||(setting.mip_steps&&!c.mip_samples)||(setting.protect_edges&&!c.edge_input_mask))return;
        }
        auto prepared=apply?bundle:arc::PolicyBundle{}; // allocation failure must leave the old policy intact
        auto targets=s.record_targets;if(!bundle.compute.empty()){targets.clear();for(const auto& p:bundle.compute)targets.push_back(p.pipeline);}
        s.bundle=std::move(prepared);s.current_bundle=s.bundle.id;s.learning_bundle=apply?arc::PolicyBundle{}:bundle;s.record_targets=std::move(targets);s.bundle_mode=true;s.heaviest_only=false;s.instrumentation=!bundle.compute.empty()||!apply;s.calibration_epoch=calibration_epoch;
        s.policy_valid_until=valid_until_frame;
        if(s.cpu_optimize!=(apply&&bundle.cpu_state_cache)){++s.cpu_generation;for(auto& view:s.cpu_views)view.address=0;s.cpu_optimize=apply&&bundle.cpu_state_cache;}
        if(calibration_epoch){s.calibration_measurement_epoch=calibration_epoch;s.calibration_expected=0;}
        s.x_rate=s.y_rate=1;s.comparison_taps=s.zero_factor=s.mip_steps=0;
        s.selected_pipeline=s.record_targets.empty()?0:s.record_targets.front();
        ++s.policy_epoch;accepted=true;
    });return accepted;
}
bool begin_calibration(const arc::PolicyBundle& bundle,std::uint64_t epoch)noexcept{
    return epoch&&configure_bundle(bundle,true,epoch);
}
bool configure_bundle_file(const wchar_t* path)noexcept{
    if(!path)return false;
    try{
        const std::filesystem::path file(path);if(!file.is_absolute()||!std::filesystem::is_regular_file(file)||std::filesystem::file_size(file)>65536)return false;
        std::ifstream input(file);const auto config=nlohmann::json::parse(input);
        auto number=[](const auto& object,const char* key,std::uint64_t fallback=0){
            const auto it=object.find(key);if(it==object.end())return fallback;
            if(!it->is_number_unsigned())throw std::runtime_error("Unsigned policy field required");return it->template get<std::uint64_t>();
        };
        if(number(config,"schema")!=1)return false;
        arc::PolicyBundle bundle;bundle.id=number(config,"id");bundle.cpu_state_cache=config.value("cpu_state_cache",false);const auto& entries=config.at("compute");
        if(!entries.is_array()||entries.size()>arc::PolicyBundle::capacity)return false;
        for(const auto& entry:entries){arc::ComputePolicy p;p.pipeline=number(entry,"pipeline");
            auto field=[&](const char* name,unsigned fallback=0){const auto value=number(entry,name,fallback);if(value>UINT_MAX)throw std::runtime_error("Policy field range");return static_cast<unsigned>(value);};
            p.x_rate=field("x_rate",1);p.y_rate=field("y_rate",1);p.comparison_taps=field("comparison_taps");p.zero_factor=field("zero_factor");p.mip_steps=field("mip_steps");p.sample_percent=field("sample_percent",100);
            p.protect_edges=entry.value("protect_edges",false);p.edge_threshold=entry.value("edge_threshold",.08f);bundle.compute.push_back(p);
        }
        const auto operation=config.value("operation",std::string("apply"));
        if(operation=="calibrate")return begin_calibration(bundle,number(config,"epoch",bundle.id));
        if(operation!="apply"&&operation!="prepare")return false;
        return configure_bundle(bundle,operation=="apply");
    }catch(...){return false;}
}
void calibration_snapshot(std::ostream& out){
    std::lock_guard lock(state().mutex);const auto& s=state();out<<"{\"active_epoch\":"<<s.calibration_epoch<<",\"measurement\":\"same_inputs_neutral_wrapper_and_original_dispatch\",\"complete_overhead_evidence\":false,\"samples\":[";bool first=true;
    for(const auto& control:s.controls)for(const auto& sample:control->calibrations()){
        if(!first)out<<',';first=false;out<<"{\"epoch\":"<<sample.epoch<<",\"pipeline\":"<<sample.pipeline<<",\"wrapped_ms\":"<<sample.wrapped_ms<<",\"original_ms\":"<<sample.original_ms<<",\"neutralize_ms\":"<<sample.neutralize_ms<<",\"upload_ms\":"<<sample.upload_ms<<'}';
    }out<<"]}";
}
std::vector<CalibrationCost> calibration_costs(std::uint64_t epoch)noexcept{
    std::vector<CalibrationCost> result;
    safe([&]{const auto& s=state();if(s.faults||s.calibration_measurement_epoch!=epoch)return;for(const auto& control:s.controls){
        for(const auto& sample:control->calibrations())if(sample.epoch==epoch&&sample.pipeline)result.push_back({sample.pipeline,epoch,sample.wrapped_ms,sample.original_ms,sample.neutralize_ms,sample.upload_ms});
    }});return result;
}
std::uint64_t calibration_sample_count(std::uint64_t epoch)noexcept{
    std::uint64_t count=UINT64_MAX;safe([&]{const auto& s=state();if(!s.faults&&s.calibration_measurement_epoch==epoch)count=s.calibration_expected;});return count;
}
void predication(ID3D12GraphicsCommandList* native,bool enabled)noexcept{if(optimizer::enabled())safe([&]{if(auto* c=recording(native))c->unpredicated=!enabled;});}
void query_scope(ID3D12GraphicsCommandList* native,bool begin)noexcept{if(enabled())safe([&]{if(auto* c=recording(native)){if(begin){if(c->query_depth<UINT_MAX)++c->query_depth;}else if(c->query_depth)--c->query_depth;else c->unpredicated=false;}});}
void control_timing_snapshot(std::ostream& out){
    std::lock_guard lock(state().mutex);GpuControl::Timing total;
    for(const auto& control:state().controls){const auto value=control->timing();total.samples+=value.samples;total.dropped+=value.dropped;total.invalid+=value.invalid;total.milliseconds+=value.milliseconds;}
    out<<"{\"enabled\":"<<(state().measure_control?"true":"false")<<",\"measurement\":\"policy_upload_copy_and_barriers_only\",\"complete_overhead_evidence\":false,\"samples\":"<<total.samples<<",\"dropped\":"<<total.dropped<<",\"invalid\":"<<total.invalid<<",\"gpu_ms\":"<<total.milliseconds<<'}';
}
void cpu_snapshot(std::ostream& out){
    out<<"{\"enabled\":"<<(cpu_timing?"true":"false")<<",\"measurement\":\"optimizer_intercept_wall_including_native_calls\",\"total_ms\":"<<cpu_ns.load()/1.e6<<",\"original_submit_ms\":"<<original_submit_ns.load()/1.e6<<",\"complete_overhead_evidence\":false,\"sites\":[";
    bool first=true;for(const auto& sample:cpu_samples){const auto name=sample.name.load();const auto calls=sample.calls.load();if(!name||!calls)continue;if(!first)out<<',';first=false;out<<"{\"name\":"<<std::quoted(name)<<",\"calls\":"<<calls<<",\"ms\":"<<sample.ns.load()/1.e6<<",\"entry_lock_ms\":"<<sample.lock_ns.load()/1.e6<<'}';}out<<"]}";
}
bool initialize()noexcept{
    bool result=true;safe([&]{auto& s=state();if(s.enabled)return;
        const auto worker_path=environment(L"ARC_OPTIMIZER_WORKER");if(worker_path.empty())return;
        s.worker=worker_path;s.compiler=environment(L"ARC_OPTIMIZER_COMPILER");s.cache=environment(L"ARC_OPTIMIZER_CACHE");
        cpu_timing=environment(L"ARC_OPTIMIZER_CPU_TIMING")==L"1";
        arc::InterceptCpuMeter::enable(cpu_timing.load());
        s.measure_control=environment(L"ARC_OPTIMIZER_GPU_CONTROL_TIMING")==L"1"||!environment(L"ARC_AUTO_CONFIG").empty();
        s.compile_on_demand=!environment(L"ARC_AUTO_CONFIG").empty()||environment(L"ARC_OPTIMIZER_LAZY_COMPILE")==L"1";
        s.cpu_optimize=environment(L"ARC_OPTIMIZER_CPU_STATE_CACHE")==L"1";
        if(!s.worker.is_absolute()||!s.compiler.is_absolute()||!s.cache.is_absolute()||!std::filesystem::is_regular_file(s.worker)||!std::filesystem::is_regular_file(s.compiler)) {result=false;return;}
        s.persistent_cache=s.cache/L"analyzed-v1";std::filesystem::create_directories(s.persistent_cache);
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
    else if(command==L"samples25"||command==L"samples50"||command==L"samples75"){s.x_rate=s.y_rate=1;s.sample_percent=command==L"samples25"?25:command==L"samples50"?50:75;accepted=true;}
    else if(command==L"mip-half"||command==L"mip1"||command==L"mip2"){s.x_rate=s.y_rate=1;s.mip_steps=command==L"mip-half"?1:command==L"mip1"?2:4;accepted=true;}
    if(accepted&&wcscmp(value,L"pcf9")!=0)s.comparison_taps=0;
    if(accepted&&wcscmp(value,L"zero")!=0)s.zero_factor=0;
    if(accepted&&!command.starts_with(L"samples"))s.sample_percent=100;
    if(accepted&&command!=L"mip-half"&&command!=L"mip1"&&command!=L"mip2")s.mip_steps=0;
    if(accepted){++s.policy_epoch;s.policy_valid_until=0;s.bundle_mode=false;s.bundle={};s.current_bundle=0;s.calibration_epoch=0;s.instrumentation=command!=L"off";s.protect_edges=command.starts_with(L"adaptive-");s.edge_threshold=edge_threshold;s.heaviest_only=heaviest;
        if(heaviest&&std::none_of(s.pipelines.begin(),s.pipelines.end(),[&](const auto& p){return p.second->id==s.selected_pipeline&&p.second->variant;})){s.selected_pipeline=0;s.cost_session=0;s.selected_cost=0;s.cost_prepared=0;}}
});return accepted;}
DescriptorWrite descriptor_write()noexcept{static const auto sample=register_cpu_sample("descriptor_write_lock");CpuMeter meter(sample);DescriptorWrite result;if(enabled())result.lock=std::unique_lock(state().descriptor_mutex);meter.locked();return result;}
void root_created(ID3D12RootSignature* native,const void* bytes,SIZE_T size)noexcept{if(!enabled()||!native||!bytes||!size||size>65536)return;safe([&]{auto& s=state();
    // CreateRootSignature may return another reference to an interned object.
    // Replacing its lifetime token would invalidate every existing PSO's root
    // identity although the native object and layout have not changed.
    if(s.roots.contains(native)||s.roots.size()>=1024)return;auto root=std::make_shared<Root>();root->id=s.next++;root->native=native;const auto* data=static_cast<const std::byte*>(bytes);root->bytes.assign(data,data+size);root->layout=std::make_shared<binding::Layout>(binding::Layout::parse(root->bytes));if(track(native,1))s.roots[native]=std::move(root);});}
void compute_created(ID3D12PipelineState* native,const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc)noexcept{if(!enabled()||!native||!desc||desc->NodeMask>1||!desc->CS.pShaderBytecode||!desc->CS.BytecodeLength||desc->CS.BytecodeLength>2*1024*1024)return;const auto profile_id=gpu_profile::pipeline_identity(native);safe([&]{auto& s=state();
    auto decline=[&](const std::string& reason){if(s.pipeline_declines.size()<64||s.pipeline_declines.contains(reason))++s.pipeline_declines[reason];};
    if(s.pipelines.contains(native))return;
    if(s.pipelines.size()>=16384||s.jobs.size()>=256||desc->CS.BytecodeLength>64u*1024u*1024u-s.queued_code_bytes){decline("pipeline_or_worker_capacity");return;}
    const auto root=s.roots.find(desc->pRootSignature);if(root==s.roots.end()){decline("unobserved_root_signature");return;}if(!root->second->layout->complete){decline("root:"+root->second->layout->rejection);return;}
    auto p=std::make_shared<Pipeline>();p->id=s.next++;p->profile_id=profile_id;p->root=root->second;check(native->GetDevice(IID_PPV_ARGS(&p->device)));if(p->device->GetNodeCount()!=1)return;
    const auto* bytes=static_cast<const std::byte*>(desc->CS.pShaderBytecode);p->code.assign(bytes,bytes+desc->CS.BytecodeLength);if(!track(native,2))return;s.pipelines[native]=p;s.queued_code_bytes+=desc->CS.BytecodeLength;
    if(s.compile_on_demand)p->reason="awaiting_measured_cost";
    else{s.jobs.push_back(p);p->queued=true;s.changed.notify_one();}
});}
void heap_created(ID3D12DescriptorHeap* native)noexcept{if(!enabled()||!native)return;safe([&]{auto& s=state();if(s.heaps.contains(native)||s.heaps.size()>=4096)return;mirror::InternalCall guard;const auto desc=native->GetDesc();Ptr<ID3D12Device> device;check(native->GetDevice(IID_PPV_ARGS(&device)));
    binding::DescriptorHeap h{s.next++,native->GetCPUDescriptorHandleForHeapStart().ptr,0,device->GetDescriptorHandleIncrementSize(desc.Type),desc.NumDescriptors,desc.Type};if(desc.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)h.gpu=native->GetGPUDescriptorHandleForHeapStart().ptr;
    if(!s.descriptors.register_heap(h.id,h.cpu,h.stride,h.count))return;if(!track(native,4)){s.descriptors.retire_heap(h.id);return;}s.heaps[native]=h;s.heaps_by_id[h.id]=h;
});}
void stream_created(ID3D12PipelineState* native,const D3D12_PIPELINE_STATE_STREAM_DESC* stream)noexcept{
    if(!enabled()||!native||!stream)return;
    try{if(const auto description=binding::compute_stream(*stream))compute_created(native,&*description);}catch(...){}
}
void signature_created(ID3D12CommandSignature* native,const D3D12_COMMAND_SIGNATURE_DESC* desc,ID3D12RootSignature* root)noexcept{
    if(!enabled()||!native||!desc||!desc->pArgumentDescs||!desc->NumArgumentDescs||desc->NumArgumentDescs>64)return;
    safe([&]{auto& s=state();if(s.signatures.contains(native)||s.signatures.size()>=4096)return;Signature signature;signature.known=true;unsigned work=0;
        if(const auto found=s.roots.find(root);found!=s.roots.end())signature.root=found->second;
        for(UINT i=0;i<desc->NumArgumentDescs;++i){const auto& arg=desc->pArgumentDescs[i];switch(arg.Type){
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:signature.compute=true;++work;break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:++work;break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:signature.resets.push_back(arg);break;
            default:signature.known=false;break;
        }}signature.known&=work==1;if(track(native,7))s.signatures.emplace(native,std::move(signature));
    });
}
void after_indirect(ID3D12GraphicsCommandList* native,ID3D12CommandSignature* signature)noexcept{
    if(!enabled())return;safe([&]{auto& s=state();auto* c=recording(native);if(!c)return;const auto found=s.signatures.find(signature);
        if(found==s.signatures.end()||!found->second.known){state_unknown(native);return;}
        const auto& info=found->second;if(!info.compute||info.resets.empty())return;
        if(!info.root||info.root!=c->root){state_unknown(native);return;}
        const std::array<UINT,64> zero{};
        for(const auto& reset:info.resets){if(reset.Type==D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT){
            if(reset.Constant.Num32BitValuesToSet>64||!c->arguments.constants(reset.Constant.RootParameterIndex,reset.Constant.DestOffsetIn32BitValues,{zero.data(),reset.Constant.Num32BitValuesToSet})){state_unknown(native);return;}
        }else{
            const auto type=reset.Type==D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW?D3D12_ROOT_PARAMETER_TYPE_CBV:reset.Type==D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW?D3D12_ROOT_PARAMETER_TYPE_SRV:D3D12_ROOT_PARAMETER_TYPE_UAV;
            const auto parameter=reset.ConstantBufferView.RootParameterIndex;const auto arg=c->arguments.raw_argument(parameter);
            if(!arg||arg->type!=type){state_unknown(native);return;}c->arguments.descriptor(parameter,type,0);
        }}
    });
}
void resource_created(ID3D12Resource* native,ID3D12Heap* heap,UINT64 offset)noexcept{if(!enabled()||!native)return;safe([&]{auto& s=state();const auto id=resource_identity(native);if(!id)return;auto& a=s.allocations.at(id);a.kind=heap?binding::AllocationKind::Placed:binding::AllocationKind::Committed;
    if(heap){auto it=s.allocation_heaps.find(heap);if(it==s.allocation_heaps.end()){if(s.allocation_heaps.size()>=4096||!track(heap,6)){a.kind=binding::AllocationKind::Unknown;return;}it=s.allocation_heaps.emplace(heap,s.next++).first;}a.heap=it->second;a.offset=offset;}
    // Committed storage is unique by construction; its padded allocation size
    // is not needed for alias checks. Placed resources do require that size.
    if(!heap){a.bytes=0;return;}
    Ptr<ID3D12Device> device;check(native->GetDevice(IID_PPV_ARGS(&device)));
    const auto& d=a.description;const std::array<UINT64,11> key{static_cast<UINT64>(d.Dimension),d.Alignment,d.Width,d.Height,d.DepthOrArraySize,d.MipLevels,static_cast<UINT64>(d.Format),d.SampleDesc.Count,d.SampleDesc.Quality,static_cast<UINT64>(d.Layout),static_cast<UINT64>(d.Flags)};
    auto cache=std::find_if(s.allocation_cache.begin(),s.allocation_cache.end(),[&](const auto& c){return c.device.Get()==device.Get();});
    if(cache==s.allocation_cache.end()&&s.allocation_cache.size()<4){s.allocation_cache.push_back({device,{}});cache=std::prev(s.allocation_cache.end());}
    if(cache!=s.allocation_cache.end()){if(const auto found=cache->sizes.find(key);found!=cache->sizes.end()){a.bytes=found->second;return;}}
    a.bytes=device->GetResourceAllocationInfo(0,1,&d).SizeInBytes;
    if(a.bytes==UINT64_MAX||!a.bytes)a.kind=binding::AllocationKind::Unknown;
    else if(cache!=s.allocation_cache.end()&&cache->sizes.size()<256)cache->sizes.emplace(key,a.bytes);
});}
void srv(ID3D12Resource* resource,const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{if(!enabled())return;safe([&]{DescriptorValue v;v.kind=1;v.resource=resource_identity(resource);if(desc){v.shape.known=!resource||v.resource;v.shape.format=desc->Format;v.shape.dimension=desc->ViewDimension;v.shape.component_mapping=desc->Shader4ComponentMapping;
    switch(desc->ViewDimension){case D3D12_SRV_DIMENSION_TEXTURE2D:v.first_mip=desc->Texture2D.MostDetailedMip;v.mips=desc->Texture2D.MipLevels;v.shape.plane=desc->Texture2D.PlaneSlice;break;
    case D3D12_SRV_DIMENSION_BUFFER:v.shape.first_element=desc->Buffer.FirstElement;v.shape.elements=desc->Buffer.NumElements;v.shape.stride=desc->Buffer.StructureByteStride;v.shape.flags=desc->Buffer.Flags;break;
    case D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE:{const auto address=desc->RaytracingAccelerationStructure.Location;
        const auto* allocation=state().buffers.resolve(state().allocations,address,1);v.shape.known=allocation!=nullptr;
        if(allocation){v.resource=allocation->id;v.shape.byte_offset=address-allocation->gpu_address;v.shape.byte_size=1;}break;}
    default:break;}}
    write_view(handle,v);remember_view(1,resource,nullptr,desc,sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC),handle);
});}
void uav(ID3D12Resource* resource,ID3D12Resource* counter,const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{if(!enabled())return;safe([&]{
    // The admitted shader contract rejects every non-2D UAV. Preserve the
    // exact CPU-view cache, but do not build unused semantic values for them.
    // Forgetting a previous typed view is essential when a slot is overwritten.
    if(desc&&desc->ViewDimension!=D3D12_UAV_DIMENSION_TEXTURE2D){state().descriptors.forget(handle.ptr);remember_view(2,resource,counter,desc,sizeof(*desc),handle);return;}
    DescriptorValue v;v.kind=2;v.resource=resource_identity(resource);v.shape.counter_resource=resource_identity(counter);if(desc){v.shape.known=(!resource||v.resource)&&(!counter||v.shape.counter_resource);v.shape.format=desc->Format;v.shape.dimension=desc->ViewDimension;
    if(desc->ViewDimension==D3D12_UAV_DIMENSION_TEXTURE2D){v.first_mip=desc->Texture2D.MipSlice;v.mips=1;v.shape.plane=desc->Texture2D.PlaneSlice;}}
    write_view(handle,v);remember_view(2,resource,counter,desc,sizeof(D3D12_UNORDERED_ACCESS_VIEW_DESC),handle);
});}
void cbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept {
    if(!enabled())return;safe([&]{DescriptorValue v;v.kind=6;
        if(desc){v.shape.byte_size=desc->SizeInBytes;v.shape.known=desc->BufferLocation==0;
            if(const auto* a=state().buffers.resolve(state().allocations,desc->BufferLocation,desc->SizeInBytes)){v.resource=a->id;v.shape.byte_offset=desc->BufferLocation-a->gpu_address;v.shape.known=true;}}
        write_view(handle,v);remember_view(6,nullptr,nullptr,desc,sizeof(D3D12_CONSTANT_BUFFER_VIEW_DESC),handle);
    });
}
void sampler(const D3D12_SAMPLER_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{if(!enabled())return;safe([&]{DescriptorValue v;v.kind=5;v.shape.known=desc!=nullptr;write_view(handle,v);});}
void copy_descriptors(UINT count,D3D12_CPU_DESCRIPTOR_HANDLE destination,D3D12_CPU_DESCRIPTOR_HANDLE source,UINT stride)noexcept{if(!enabled())return;safe([&]{auto& ledger=state().descriptors;if(!stride||count>65536){state().descriptors.forget_all();for(auto& view:state().cpu_views)view.address=0;++state().admission_reasons["descriptor_copy_capacity"];return;};
    if(state().cpu_optimize){if(count>state().cpu_views.size()){for(auto& view:state().cpu_views)view.address=0;}else for(UINT i=0;i<count;++i)forget_raw_view(destination.ptr+UINT64(i)*stride);}
    if(count==1){if(!ledger.copy_one(destination.ptr,source.ptr))ledger.forget(destination.ptr);return;}std::vector<std::optional<DescriptorValue>> values;values.reserve(count);for(UINT i=0;i<count;++i)values.push_back(ledger.read(source.ptr+UINT64(i)*stride));for(UINT i=0;i<count;++i){const auto address=destination.ptr+UINT64(i)*stride;if(values[i]){if(!ledger.write(address,*values[i]))ledger.forget(address);}else ledger.forget(address);}});}
void descriptor_ranges(UINT dc,const D3D12_CPU_DESCRIPTOR_HANDLE* dst,const UINT* ds,UINT sc,const D3D12_CPU_DESCRIPTOR_HANDLE* src,const UINT* ss,UINT stride)noexcept{if(!enabled())return;safe([&]{auto unavailable=[&]{state().descriptors.forget_all();for(auto& view:state().cpu_views)view.address=0;++state().admission_reasons["descriptor_range_capacity"];};if(dc>65536||sc>65536||!stride||(!dst&&dc)||(!src&&sc)){unavailable();return;}std::vector<std::uint64_t> addresses;std::vector<std::optional<DescriptorValue>> values;auto& ledger=state().descriptors;
    for(UINT r=0;r<dc;++r){const auto count=ds?ds[r]:1;if(count>65536||addresses.size()+count>65536){unavailable();return;};for(UINT i=0;i<count;++i)addresses.push_back(dst[r].ptr+UINT64(i)*stride);}
    for(UINT r=0;r<sc;++r){const auto count=ss?ss[r]:1;if(count>65536||values.size()+count>65536){unavailable();return;};for(UINT i=0;i<count;++i)values.push_back(ledger.read(src[r].ptr+UINT64(i)*stride));}
    for(std::size_t i=0;i<addresses.size();++i){if(state().cpu_optimize)forget_raw_view(addresses[i]);if(addresses.size()==values.size()&&values[i]){if(!ledger.write(addresses[i],*values[i]))ledger.forget(addresses[i]);}else ledger.forget(addresses[i]);}
});}
void begin(ID3D12GraphicsCommandList* native,ID3D12PipelineState* pipeline)noexcept{if(!enabled()||!native)return;safe([&]{auto& s=state();auto it=s.commands.find(native);if(it==s.commands.end()){if(s.commands.size()>=256||!track(native,3))return;it=s.commands.emplace(native,Recording{}).first;}
    auto& c=it->second;++s.cpu_lookup_epoch;if(c.cpu_cache){c.cpu_cache->cache.invalidate();c.cpu_cache->usable=true;}c.pipeline=pipeline;c.root.reset();c.arguments.reset();c.heaps.clear();c.control.reset();c.uses.clear();c.valid=true;c.unpredicated=true;c.query_depth=0;c.closed=c.epilogue=c.render_pass=c.arguments_uncertain=false;
});}
void pipeline(ID3D12GraphicsCommandList* native,ID3D12PipelineState* pipeline)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native))c->pipeline=pipeline;});}
void heaps(ID3D12GraphicsCommandList* native,UINT count,ID3D12DescriptorHeap*const* heaps)noexcept{if(!enabled())return;safe([&]{auto* c=recording(native);if(!c)return;if(count>2||(!heaps&&count)){c->valid=false;return;}std::array<std::uint64_t,2> next{};
    for(UINT i=0;i<count;++i){auto it=state().heaps.find(heaps[i]);if(it==state().heaps.end()){heap_created(heaps[i]);it=state().heaps.find(heaps[i]);}if(it==state().heaps.end()){c->valid=false;return;}next[i]=it->second.id;}
    if(c->heaps.size()!=count||!std::equal(c->heaps.begin(),c->heaps.end(),next.begin())){c->arguments.invalidate_tables();c->heaps.assign(next.begin(),next.begin()+count);}
});}
void root(ID3D12GraphicsCommandList* native,ID3D12RootSignature* root)noexcept{if(!enabled())return;safe([&]{auto* c=recording(native);if(!c)return;const auto it=state().roots.find(root);c->root=it==state().roots.end()?nullptr:it->second;c->arguments.signature(c->root?c->root->id:0,c->root?c->root->layout:nullptr);});}
void table(ID3D12GraphicsCommandList* native,UINT parameter,D3D12_GPU_DESCRIPTOR_HANDLE handle)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native))c->arguments.table(parameter,handle);});}
void constants(ID3D12GraphicsCommandList* native,UINT parameter,UINT count,const UINT* values,UINT offset)noexcept{if(!enabled())return;safe([&]{if((!values&&count)||count>64)return;if(auto* c=recording(native))c->arguments.constants(parameter,offset,{values,count});});}
void descriptor(ID3D12GraphicsCommandList* native,UINT parameter,D3D12_ROOT_PARAMETER_TYPE type,UINT64 address)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native))c->arguments.descriptor(parameter,type,address);});}
void invalidate(ID3D12GraphicsCommandList* native)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native)){c->valid=false;if(c->cpu_cache)c->cpu_cache->usable=false;}});}
void state_unknown(ID3D12GraphicsCommandList* native)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native)){if(c->cpu_cache)c->cpu_cache->cache.invalidate();c->arguments.reset();c->root.reset();c->pipeline=nullptr;c->arguments_uncertain=true;c->unpredicated=false;}});}
void render_pass(ID3D12GraphicsCommandList* native,bool begin,D3D12_RENDER_PASS_FLAGS flags)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native)){c->render_pass=begin;if(flags&(D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS|D3D12_RENDER_PASS_FLAG_RESUMING_PASS))c->valid=false;}});}
void invalidate_all()noexcept{if(!enabled())return;safe([&]{state().x_rate=state().y_rate=1;state().comparison_taps=0;state().zero_factor=0;state().bundle={};state().cpu_optimize=false;++state().cpu_lookup_epoch;for(auto& [native,c]:state().commands){(void)native;c.valid=false;if(c.cpu_cache)c.cpu_cache->usable=false;}});}
bool dispatch(ID3D12GraphicsCommandList* native,UINT x,UINT y,UINT z)noexcept{if(!enabled())return false;bool changed=false;safe([&]{auto& s=state();auto* c=recording(native);
    if(!c){++s.dispatch_declines[0];return;}if(!c->valid){++s.dispatch_declines[1];return;}if(c->closed){++s.dispatch_declines[2];return;}if(c->render_pass){++s.dispatch_declines[3];return;}if(!c->root){++s.dispatch_declines[4];return;}if(c->uses.size()>=GpuControl::capacity){++s.dispatch_declines[5];return;}
    // Rebinding the same native root after indirect/bundle/opaque work does not
    // clear its native arguments. Never replace that root unless every value
    // needed to preserve its state has been observed again, even in neutral mode.
    if(c->arguments_uncertain){const auto layout=c->arguments.layout();bool known=layout&&layout->complete;if(known)for(UINT i=0;i<layout->parameters.size();++i){const auto a=c->arguments.raw_argument(i);if(!a||(!a->initialized&&!(a->observed&&(a->type==D3D12_ROOT_PARAMETER_TYPE_CBV||a->type==D3D12_ROOT_PARAMETER_TYPE_SRV||a->type==D3D12_ROOT_PARAMETER_TYPE_UAV)))){known=false;break;}}
        if(!known){++s.dispatch_declines[11];return;}c->arguments_uncertain=false;}
    const auto p=s.pipelines.find(c->pipeline);if(p==s.pipelines.end()){++s.dispatch_declines[6];return;}if(!p->second->variant){++s.dispatch_declines[7];return;}if(p->second->root!=c->root){++s.dispatch_declines[8];return;}
    if(!s.instrumentation){++s.dispatch_declines[9];return;}if((s.heaviest_only&&p->second->id!=s.selected_pipeline)||(s.bundle_mode&&std::find(s.record_targets.begin(),s.record_targets.end(),p->second->id)==s.record_targets.end())){++s.dispatch_declines[10];return;}
    const auto variant=p->second->variant;
    const bool spatial_prepass=variant->contract.execution_marker&&variant->contract.edge_input_mask;
    if(spatial_prepass&&(!c->unpredicated||c->query_depth)){++s.dispatch_declines[11];return;}
    if(!c->control){for(auto& control:s.controls)if(control.use_count()==1&&control->device()==p->second->device.Get()&&control->ready()){control->keep_alive.clear();control->begin_recording();c->control=control;break;}if(!c->control){++s.pool_misses;return;}}
    const bool calibration=s.calibration_epoch&&c->unpredicated&&!c->query_depth&&c->control->calibration_available();
    const UINT slot=static_cast<UINT>(c->uses.size());std::shared_ptr<SpatialProbeGpu> probe;if(s.spatial_learning&&variant->probe_pipeline&&c->unpredicated&&!c->query_depth){const auto found=s.probe_pools.find(p->second->device.Get());if(found!=s.probe_pools.end())probe=found->second;}
    c->uses.push_back({variant,c->arguments,c->heaps,x,y,z,calibration,probe});c->control->keep_alive.push_back(variant);
    mirror::InternalCall internal;if(calibration)c->control->calibration_mark(native,slot,0);
    native->SetComputeRootSignature(variant->root.Get());replay_arguments(native,c->arguments);native->SetComputeRootConstantBufferView(static_cast<UINT>(c->root->layout->parameters.size()),c->control->address(slot));native->SetPipelineState(variant->pipeline.Get());
    if(variant->contract.execution_marker)native->SetComputeRootUnorderedAccessView(static_cast<UINT>(c->root->layout->parameters.size()+1),c->control->marker_address(slot));
    if(spatial_prepass){
        native->SetComputeRootConstantBufferView(static_cast<UINT>(c->root->layout->parameters.size()),c->control->prepass_address(slot));
        native->SetPipelineState(variant->spatial_pipeline.Get());const auto gx=2*((16+variant->contract.threads[0]*2-1)/(variant->contract.threads[0]*2)),gy=2*((16+variant->contract.threads[1]*2-1)/(variant->contract.threads[1]*2));native->Dispatch((x+gx-1)/gx,(y+gy-1)/gy,z);c->control->marker_barrier(native);
        native->SetPipelineState(variant->pipeline.Get());native->SetComputeRootConstantBufferView(static_cast<UINT>(c->root->layout->parameters.size()),c->control->address(slot));
    }
    {arc::InterceptCpuMeter::Native application_work;native->Dispatch(x,y,z);}
    if(variant->contract.execution_marker)c->control->marker_barrier(native);
    if(probe){c->control->keep_alive.push_back(probe);c->control->probe_predicate(native,slot);
        native->SetComputeRootSignature(variant->probe_root.Get());replay_arguments(native,c->arguments);native->SetPipelineState(variant->probe_pipeline.Get());native->SetComputeRootUnorderedAccessView(static_cast<UINT>(c->root->layout->parameters.size()+1),c->control->marker_address(slot));
        for(unsigned candidate=0;candidate<2;++candidate){native->SetComputeRootConstantBufferView(static_cast<UINT>(c->root->layout->parameters.size()),c->control->probe_address(slot,candidate!=0));native->SetComputeRootUnorderedAccessView(static_cast<UINT>(c->root->layout->parameters.size()+2),candidate?probe->candidate_address():probe->reference_address());native->Dispatch(x,y,z);}
        probe->record_compare(native,c->control->probe_address(slot,true),c->control->marker_address(slot),variant->contract.probe_outputs,true);native->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
    native->SetComputeRootSignature(c->root->native);replay_arguments(native,c->arguments);native->SetPipelineState(c->pipeline);changed=true;++s.modified_dispatches;
    if(calibration){
        c->control->calibration_mark(native,slot,1);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;native->ResourceBarrier(1,&barrier);
        c->control->calibration_predicate(native,slot);c->control->calibration_mark(native,slot,2);native->Dispatch(x,y,z);c->control->calibration_mark(native,slot,3);
        native->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
});return changed;}
void close(ID3D12GraphicsCommandList* native)noexcept{if(!enabled())return;safe([&]{if(auto* c=recording(native)){if(c->control&&c->valid&&!c->closed&&!c->render_pass&&!c->uses.empty()){mirror::InternalCall internal;c->control->record_neutralize(native);c->epilogue=true;}c->closed=true;if(c->cpu_cache)c->cpu_cache->usable=false;}});}
bool execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists)noexcept {
    if(!enabled()||!queue||(!lists&&count))return false;
    static const auto sample=register_cpu_sample("execute");CpuMeter meter(sample);
    auto& s=state();std::lock_guard descriptors(s.descriptor_mutex);std::unique_lock lock(s.mutex);meter.locked();
    std::array<std::shared_ptr<GpuControl>,64> controls{};unsigned used=0;
    struct Staged {std::shared_ptr<GpuControl> control;std::array<ControlValue,GpuControl::capacity> values,probes;unsigned count{};std::vector<std::shared_ptr<SpatialProbeGpu>> pools;};
    std::vector<Staged> staged;const auto binding_revision=s.binding_evidence.revision();
    auto fault=[&](const char* message){++s.faults;s.x_rate=s.y_rate=1;s.comparison_taps=0;s.zero_factor=0;try{s.last_error=message;}catch(...) {}};
    auto remember=[&](const std::shared_ptr<GpuControl>& c){for(unsigned j=0;j<used;++j)if(controls[j]==c)return false;if(used>=controls.size()){fault("control capacity");return false;}controls[used++]=c;return true;};
    if(count>1024){
        // An oversized native batch still executes exactly once. Conservatively
        // order every retained control, with neutral policy and bounded storage.
        for(auto& c:s.controls)if(remember(c))try{mirror::InternalCall internal;c->prepare(queue,{});}catch(const std::exception& e){fault(e.what());}
    }else for(UINT i=0;i<count;++i){
        const auto found=s.commands.find(reinterpret_cast<ID3D12GraphicsCommandList*>(lists[i]));if(found==s.commands.end())continue;
        auto& c=found->second;if(!c.control||!remember(c.control))continue;
        try{
            std::array<ControlValue,GpuControl::capacity> values{},probes{};std::vector<std::shared_ptr<SpatialProbeGpu>> pools;
            if(!s.faults&&c.valid&&c.closed&&c.epilogue&&(!s.policy_valid_until||s.current_frame<s.policy_valid_until)&&(!s.required_queue||s.required_queue.Get()==queue))for(unsigned n=0;n<c.uses.size();++n){
                const auto& use=c.uses[n];
                const auto* requested=s.bundle_mode?s.bundle.find(use.variant->id):nullptr;
                const bool learning=s.bundle_mode&&!requested&&s.learning_bundle.find(use.variant->id);
                if(learning)requested=s.learning_bundle.find(use.variant->id);
                // Neutral and unselected cached recordings require no descriptor
                // proofs. Their GPU epilogue restores neutral control values.
                if(!s.sample_state&&((s.bundle_mode&&!requested)||(!s.bundle_mode&&(s.heaviest_only&&use.variant->id!=s.selected_pipeline))||!s.instrumentation))continue;
                const arc::ComputePolicy setting=requested?*requested:arc::ComputePolicy{use.variant->id,s.x_rate,s.y_rate,s.comparison_taps,s.zero_factor,s.mip_steps,s.protect_edges,s.edge_threshold,s.sample_percent};
                std::vector<binding::DescriptorHeap> heaps;
                for(auto id:use.heaps){const auto h=s.heaps_by_id.find(id);if(h!=s.heaps_by_id.end())heaps.push_back(h->second);}
                if(s.sample_state&&use.variant->id==s.selected_pipeline){mirror::InternalCall internal;std::map<std::pair<unsigned,unsigned>,UniformMemory> views;
                    FrameStateSample sample;sample.pipeline=use.variant->id;sample.submission=s.last_frame_state.submission+1;sample.keys=use.variant->state_reads;
                    for(unsigned index=0;index<sample.keys.size();++index){const auto& key=sample.keys[index];const auto value=read_uniform_memory(use,heaps,views,key[0],key[1],key[2]);sample.words.push_back(value.words);sample.valid.push_back(value.valid_mask&use.variant->state_masks[index]);}s.last_frame_state=std::move(sample);
                }
                auto prove=[&]{
                    auto& stats=*use.variant;const auto started=std::chrono::steady_clock::now();++stats.uniform_attempts;
                    const auto usage=uniform_usage(use,heaps);stats.uniform_steps+=usage.steps;stats.uniform_reason=usage.reason;
                    stats.uniform_cpu_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
                    if(usage.complete)++stats.uniform_proven;return usage;
                };
                // A previous sparse-binding proof is revalidated against this
                // submission first, avoiding a redundant guaranteed-to-fail
                // walk over unused descriptors in the full declared array.
                const bool needs_proof=use.variant->cached_usage.complete||std::any_of(use.variant->contract.resources.begin(),use.variant->contract.resources.end(),[](const auto& r){return r.count==UINT_MAX;});
                shader::ResourceUsage usage;if(needs_proof&&use.variant->access)usage=prove();
                auto admitted=binding::admit_compute(use.arguments,use.variant->contract,s.descriptors,heaps,s.allocations,use.x,use.y,use.z,usage.complete?&usage:nullptr,&s.buffers);
                if(!needs_proof&&!admitted.admitted&&admitted.reason=="unknown_descriptor"&&admitted.binding_class==0&&use.variant->access){
                    usage=prove();if(usage.complete)admitted=binding::admit_compute(use.arguments,use.variant->contract,s.descriptors,heaps,s.allocations,use.x,use.y,use.z,&usage,&s.buffers);
                }
                if(admitted.admitted&&s.bundle_mode&&(s.bundle.id||s.learning_bundle.id)&&requested){
                    std::vector<arc::DescriptorValue> views;
                    for(const auto& input:admitted.inputs)if(input.allocation.description.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D)views.push_back(input.view);
                    for(const auto& output:admitted.outputs)views.push_back(output.view);
                    if(!s.binding_evidence.observe(learning?s.learning_bundle.id:s.bundle.id,use.variant->id,std::move(views))){admitted.admitted=false;admitted.reason="policy_texture_generation_changed";}
                }
                if(admitted.admitted&&requested&&!use.variant->state_reads.empty()&&use.variant->scene_frame!=s.current_frame){
                    mirror::InternalCall internal;auto& variant=*use.variant;std::map<std::pair<unsigned,unsigned>,UniformMemory> views;arc::UniformChange change;const bool continuous=variant.scene_frame&&s.current_frame>variant.scene_frame&&s.current_frame-variant.scene_frame<=4;
                    for(unsigned index=0;index<variant.state_reads.size();++index){const auto& key=variant.state_reads[index];auto words=read_uniform_memory(use,heaps,views,key[0],key[1],key[2]);words.valid_mask&=variant.state_masks[index];arc::accumulate_uniform_change(variant.scene_words[index],words,change);variant.scene_words[index]=words;}
                    variant.scene_frame=s.current_frame;if(continuous&&change.changed()){s.binding_evidence.retire_pipeline(variant.id);s.approved_policy=0;admitted.admitted=false;admitted.reason="large_readable_uniform_change";LARGE_INTEGER timestamp{};QueryPerformanceCounter(&timestamp);if(s.scene_event_count<s.scene_events.size())s.scene_events[s.scene_event_count++]={s.current_frame.load(),variant.id,change.large,std::uint64_t(timestamp.QuadPart)};else ++s.scene_events_dropped;}
                }
                auto& v=*use.variant;++v.attempts;v.admitted+=admitted.admitted;v.last_reason=admitted.reason;v.binding_class=admitted.binding_class;v.binding_register=admitted.binding_register;v.binding_space=admitted.binding_space;
                if(s.history.size()<256||s.history.contains(v.id))s.history[v.id]=v;
                if(s.admission_reasons.size()<64||s.admission_reasons.contains(admitted.reason))++s.admission_reasons[admitted.reason];
                if(admitted.admitted&&(s.bundle_mode?requested!=nullptr:(!s.heaviest_only||use.variant->id==s.selected_pipeline))){
                    values[n]={setting.x_rate,setting.y_rate,admitted.width,admitted.height,use.variant->contract.comparison_filter_groups?setting.comparison_taps:0,use.variant->contract.zero_factor_regions?setting.zero_factor:0};
                    values[n].reserved=use.variant->id;
                    if(use.variant->contract.mip_samples)values[n].mip_steps=setting.mip_steps;if(use.variant->contract.sample_loops)values[n].sample_percent=setting.sample_percent;
                    if(setting.protect_edges){unsigned mask=0,selected=0;std::vector<std::uint64_t> key{use.variant->id,admitted.width,admitted.height,setting.x_rate,setting.y_rate,setting.comparison_taps,setting.zero_factor,setting.mip_steps,setting.sample_percent,s.sensitivity_generation};
                        auto key_view=[&](const DescriptorValue& v){const auto& d=v.shape;key.insert(key.end(),{v.resource,v.kind,v.first_mip,v.mips,d.format,d.dimension,d.first_slice,d.slices,d.plane,d.flags,d.elements,d.stride,d.component_mapping,d.byte_offset,d.byte_size,d.first_element,d.counter_resource,std::uint64_t(d.known)});};
                        for(const auto& output:admitted.outputs)key_view(output.view);
                        for(const auto& input:admitted.inputs){const auto& r=input.contract;const auto& d=input.allocation.description;const auto& v=input.view;
                            if(selected>=4||r.resource_class!=0||r.range_id>=31||!(use.variant->contract.edge_input_mask&(1u<<r.range_id))||v.shape.dimension!=D3D12_SRV_DIMENSION_TEXTURE2D||v.first_mip>=d.MipLevels||v.first_mip>=32)continue;
                            if(std::max<UINT64>(1,d.Width>>v.first_mip)==admitted.width&&std::max(1u,d.Height>>v.first_mip)==admitted.height){mask|=1u<<r.range_id;++selected;key.push_back(r.range_id);key.push_back(input.allocation.id);key_view(v);}
                        }
                        if(mask){values[n].edge_sources=0x80000000u|mask;values[n].edge_threshold=setting.edge_threshold;
                            auto model_key=key;const auto found=s.spatial_keys.find(key);if(found!=s.spatial_keys.end())values[n].spatial_key=found->second;else if(s.spatial_keys.size()<4096)values[n].spatial_key=s.spatial_keys.emplace(std::move(key),s.next++).first->second;
                            const bool screen=!admitted.outputs.empty()&&std::all_of(admitted.outputs.begin(),admitted.outputs.end(),[&](const auto& output){return s.presentation_resources.contains(output.allocation.id);});
                            if(screen){values[n].spatial_flags|=4;if(s.center_priority)values[n].spatial_center=1;}
                            const auto rate=std::max({2u,setting.x_rate,setting.y_rate});const auto tile_x=use.variant->contract.threads[0]*rate,tile_y=use.variant->contract.threads[1]*rate;values[n].spatial_tile_width=tile_x*((16+tile_x-1)/tile_x);values[n].spatial_tile_height=tile_y*((16+tile_y-1)/tile_y);
                            const auto frame=s.current_frame.load(std::memory_order_relaxed);if(frame&&frame<UINT_MAX)values[n].spatial_frame=UINT(frame+1);
                            while(UINT64((admitted.width+values[n].spatial_tile_width-1)/values[n].spatial_tile_width)*((admitted.height+values[n].spatial_tile_height-1)/values[n].spatial_tile_height)>8192){values[n].spatial_tile_width*=2;values[n].spatial_tile_height*=2;}
                            if(s.spatial_learning){
                                auto context=s.sensitivity.find(model_key);
                                if(context==s.sensitivity.end()&&s.sensitivity.size()>=128){const auto oldest=std::min_element(s.sensitivity.begin(),s.sensitivity.end(),[](const auto& a,const auto& b){return a.second->last_seen<b.second->last_seen;});if(oldest!=s.sensitivity.end()&&frame>oldest->second->last_seen+600)s.sensitivity.erase(oldest);}
                                if(context==s.sensitivity.end()&&s.sensitivity.size()<128)context=s.sensitivity.emplace(std::move(model_key),std::make_shared<SensitivityContext>(s.next++,s.sensitivity_generation)).first;
                                if(context!=s.sensitivity.end()){
                                    auto model=context->second;model->last_seen=frame;values[n].model_key=model->id;values[n].model_limit=s.spatial_limit*std::clamp(setting.edge_threshold,0.f,1.f);values[n].spatial_flags|=32;c.control->model(n,model->table);
                                    if(s.training_pipeline==use.variant->id&&s.training_recipe.x_rate==setting.x_rate&&s.training_recipe.y_rate==setting.y_rate&&s.training_recipe.mip_steps==setting.mip_steps&&s.training_recipe.comparison_taps==setting.comparison_taps)s.training_models.insert(model->id);
                                    if(use.probe&&!s.probes_paused&&!s.sample_state&&!s.calibration_epoch&&s.required_queue.Get()==queue&&queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT&&std::count(lists,lists+count,lists[i])==1&&frame>=s.last_probe_frames[use.variant->id]+31&&use.probe->available()){
                                        const auto area=UINT64(values[n].spatial_tile_width)*values[n].spatial_tile_height;
                                        const auto tiles=UINT64((admitted.width+values[n].spatial_tile_width-1)/values[n].spatial_tile_width)*((admitted.height+values[n].spatial_tile_height-1)/values[n].spatial_tile_height);
                                        unsigned stride=16;while(stride<8192&&((tiles+stride-1)/stride)*area*use.variant->contract.probe_outputs*32>use.probe->capacity())stride*=2;
                                        const auto epoch=s.next++;
                                        if(area<=65536&&tiles<=8192&&((tiles+stride-1)/stride)*area*use.variant->contract.probe_outputs*32<=use.probe->capacity()&&use.probe->reserve(epoch)){
                                            values[n].probe_epoch=epoch;values[n].probe_stride=stride;values[n].probe_phase=model->probes%stride;probes[n]=values[n];const bool noise=model->probes%4==0;
                                            if(noise){probes[n].x=probes[n].y=1;probes[n].mip_steps=probes[n].comparison_taps=probes[n].zero_factor=0;}
                                            s.pending_probes[use.probe.get()]={model,frame+1,epoch,noise};s.last_probe_frames[use.variant->id]=frame;pools.push_back(use.probe);
                                        }
                                    }
                                }else values[n]=ControlValue{};
                            }
                            if(n==0&&s.spatial_capture_requested){values[n].spatial_flags|=8;s.spatial_capture_requested=false;}
                        }else values[n]=ControlValue{};
                    }
                    if(learning){values[n].sample_percent=100;values[n].x=values[n].y=1;values[n].comparison_taps=values[n].zero_factor=values[n].mip_steps=0;}
                    const bool effective=values[n].sample_percent!=100||values[n].x>1||values[n].y>1||values[n].comparison_taps||values[n].zero_factor||values[n].mip_steps;
                    if(effective&&s.sample_state&&use.variant->contract.execution_marker&&!s.calibration_epoch){values[n].proof_epoch=s.policy_epoch;values[n].proof_pipeline=use.variant->id;}
                    if(s.calibration_epoch){
                        // Both dispatches must compute the original result.
                        // Enabled edge validation still runs, giving its cost
                        // at full invocation density rather than hiding it.
                        values[n].sample_percent=100;values[n].x=values[n].y=1;values[n].comparison_taps=values[n].zero_factor=values[n].mip_steps=0;
                        if(use.calibration&&(!setting.protect_edges||values[n].edge_sources)&&std::count(lists,lists+count,lists[i])==1){values[n].calibration=s.calibration_epoch;values[n].calibration_pipeline=use.variant->id;}
                    }
                }
            }
            staged.push_back({c.control,std::move(values),std::move(probes),static_cast<unsigned>(c.uses.size()),std::move(pools)});
        }catch(const std::exception& e){fault(e.what());try{mirror::InternalCall internal;c.control->prepare(queue,{});}catch(...) {}}
        catch(...){fault("submission preparation failed");try{mirror::InternalCall internal;c.control->prepare(queue,{});}catch(...) {}}
    }
    // Validate the complete submitted bundle BEFORE uploading any controls.
    // One new texture generation neutralizes every affected recording in this
    // batch, including records visited before the invalid binding was found.
    const bool invalid_bindings=s.binding_evidence.revision()!=binding_revision;
    for(auto& pending:staged)try{
        if(invalid_bindings||s.faults){pending.values={};for(auto& pool:pending.pools){pool->cancel_unsubmitted();s.pending_probes.erase(pool.get());}pending.pools.clear();}
        mirror::InternalCall internal;auto* helper=pending.control->prepare(queue,{pending.values.data(),pending.count},{pending.probes.data(),pending.count});
        if(helper){queue->ExecuteCommandLists(1,&helper);if(s.bundle.id&&s.bundle.id==s.approved_policy&&s.current_frame<s.approved_until&&!s.calibration_epoch&&std::any_of(pending.values.begin(),pending.values.begin()+pending.count,[](const auto& value){return value.x>1||value.y>1||value.comparison_taps||value.zero_factor||value.mip_steps||value.sample_percent!=100;}))s.approved_submission_frame=s.current_frame.load();++s.coarse_submissions;s.last_active_epoch=s.policy_epoch;
            if(s.calibration_epoch)for(const auto& value:pending.values)if(value.calibration==s.calibration_epoch)++s.calibration_expected;
        }else{++s.neutral_submissions;for(auto& pool:pending.pools){pool->cancel_unsubmitted();s.pending_probes.erase(pool.get());}pending.pools.clear();}
    }catch(const std::exception& e){fault(e.what());}catch(...){fault("control upload failed");}
    // Never return false after submitting a helper: the original batch and all
    // retirement signals remain owned by this function even on preparation error.
    // VRS prepares its own independent control images and owns the same single
    // original submission when present. Neither actuator replays application
    // work; compute retirement follows that one combined submission.
    // The driver may block here. Unrelated recording threads must not wait on
    // the optimizer state mutex for the duration of that native submission.
    // Descriptor writes/submissions remain serialized; shared ownership keeps
    // controls out of the reuse pool until their retirement signal is issued.
    lock.unlock();
    if(!mirror::execute(queue,count,lists)){mirror::InternalCall internal;
        arc::InterceptCpuMeter::Native application_work;
        if(meter.outer){const auto start=CpuMeter::Clock::now();queue->ExecuteCommandLists(count,lists);original_submit_ns.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(CpuMeter::Clock::now()-start).count()),std::memory_order_relaxed);}
        else queue->ExecuteCommandLists(count,lists);
    }
    lock.lock();
    {mirror::InternalCall internal;
        for(auto& pending:staged)for(auto& pool:pending.pools)try{pool->submitted(queue);}catch(const std::exception& e){fault(e.what());}
        for(unsigned i=0;i<used;++i)try{controls[i]->submitted(queue);}catch(const std::exception& e){fault(e.what());}}
    return true;
}
bool control_costs_enabled()noexcept{return enabled()&&state().measure_control;}
void collect()noexcept{
    if(!enabled())return;bool need_costs=false;
    safe([&]{auto& s=state();mirror::InternalCall internal;for(auto& control:s.controls){control->collect_timing();control->release_completed_queue();}for(unsigned i=0;i<s.scene_event_count;++i)event({{"phase","uniform_scene_change"},{"frame",s.scene_events[i][0]},{"pipeline",s.scene_events[i][1]},{"changed_components",s.scene_events[i][2]},{"event_qpc",s.scene_events[i][3]},{"meaning","readable_float_state_proxy_not_semantic_visibility"}});s.scene_event_count=0;for(auto& [device,pool]:s.probe_pools)if(auto observation=pool->collect()){
        const auto pending=s.pending_probes.find(pool.get());if(pending==s.pending_probes.end())continue;auto evidence=pending->second;s.pending_probes.erase(pending);auto& context=*evidence.context;
        if(context.generation!=s.sensitivity_generation||observation->epoch!=evidence.epoch||observation->frame!=evidence.frame||observation->errors[1])continue;
        for(unsigned bin=0;bin<256;++bin){const auto& b=observation->bins[bin];context.observed_tiles[bin]=context.observed_tiles[bin]*.8+b.valid_tiles+b.invalid_tiles;if(b.valid_tiles||b.invalid_tiles){const auto error=std::bit_cast<float>(b.error_bits);const bool valid=b.invalid_tiles==0&&std::isfinite(error)&&error>=0;context.learner.observe(context.id,evidence.frame,bin,valid?error:0.f,evidence.noise,valid);}}
        ++context.probes;context.table=context.learner.snapshot();event({{"phase","spatial_probe"},{"model",context.id},{"frame",evidence.frame},{"epoch",evidence.epoch},{"neutral",evidence.noise},{"unknown_tiles",observation->errors[0]},{"probes",context.probes}});
    }need_costs=s.heaviest_only&&(!s.selected_pipeline||s.cost_prepared!=s.prepared);});
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
