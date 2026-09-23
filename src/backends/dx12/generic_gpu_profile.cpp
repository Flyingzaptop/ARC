#include "generic_cpu_workers.hpp"
#include "generic_background_budget.hpp"
#include "arc/bottleneck_router.hpp"
#include "arc/intercept_cpu_meter.hpp"
#include "generic_gpu_profile.hpp"
#include "generic_hook_control.hpp"
#include "generic_command_mirror.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <stdexcept>

namespace arc::dx12::gpu_profile {
namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
using Clock=std::chrono::steady_clock;
constexpr unsigned max_spans=128,max_records=256,max_jobs=512,max_rows=16384;
struct Module {HMODULE handle{};~Module(){if(handle)FreeLibrary(handle);}};
struct Binding {UINT type{},slot{},space{},count{},dimension{};};
std::atomic<std::size_t> captured_code_bytes{};
struct CapturedCode {
    std::vector<std::byte> bytes;
    ~CapturedCode(){captured_code_bytes.fetch_sub(bytes.size());}
};
std::shared_ptr<const CapturedCode> capture_code(const D3D12_SHADER_BYTECODE& code){
    // Explicit diagnostic opt-in, bounded in memory. File IO stays in the
    // report worker, not in PSO creation or any render command hook.
    static const bool enabled=[](){wchar_t value[8]{};return GetEnvironmentVariableW(L"ARC_CAPTURE_SHADER_CODE",value,8)==1&&value[0]==L'1';}();
    if(!enabled||!code.pShaderBytecode||!code.BytecodeLength||code.BytecodeLength>2*1024*1024)return {};
    auto result=std::make_shared<CapturedCode>();
    constexpr std::size_t budget=64*1024*1024;
    const auto old=captured_code_bytes.fetch_add(code.BytecodeLength);
    if(old>budget-code.BytecodeLength){captured_code_bytes.fetch_sub(code.BytecodeLength);return {};}
    try{const auto* start=static_cast<const std::byte*>(code.pShaderBytecode);result->bytes.assign(start,start+code.BytecodeLength);}
    catch(...){captured_code_bytes.fetch_sub(code.BytecodeLength);throw;}
    return result;
}
struct Shader {
    std::string hash;SIZE_T bytes{};UINT threads[3]{},instructions{},barriers{},atomics{};UINT64 requires_flags{};
    bool reflected{},bindings_truncated{},unbounded{};std::vector<Binding> bindings;
    std::shared_ptr<const CapturedCode> captured;
};
struct Pipeline {std::uint64_t id{};bool compute{},depth_only{};UINT render_targets{};Shader shader,vertex_shader;bool extra_geometry_stages{};std::string vertex_hash;};
struct Span {
    unsigned kind{}; // 0: color raster, 1: depth-only raster, 2: compute, 3: opaque, 4: unknown raster
    std::shared_ptr<Pipeline> pipeline;
    std::uint64_t calls{},items{},indirect_calls{},indirect_max_commands{};UINT dispatch[3]{};bool mixed_dispatch{},mixed_pipeline{};
};
struct Row {Span span;std::uint64_t recording{},frame{},queue{},submission{},start{},end{},frequency{};};
struct Session {
    std::uint64_t id{};
    std::filesystem::path path;Clock::time_point deadline,drain_deadline;
    UINT wanted{},presents{};bool stopped{},written{},publishing{},export_failed{},timed_out{};
    std::uint64_t observed_events{},trace_bytes{};
    std::uint64_t recorded{},submitted{},declined{},unsupported{},overwritten{},faults{},dropped{},abandoned{};
    std::vector<Row> rows;
    std::uint64_t cpu_begin{},cpu_end{},qpc_begin{},qpc_end{},completed_tick{};DWORD present_thread{};void* swapchain{};bool cpu_failed{},mixed_present{},pending_publication{};
};
struct Recording {
    Ptr<ID3D12Device> device;Ptr<ID3D12QueryHeap> queries;Ptr<ID3D12Resource> readback;
    std::shared_ptr<Session> session;std::shared_ptr<Pipeline> current;
    std::vector<Span> spans;std::uint64_t id{},frame{},serial{};
    bool open_span{},closed{},render_pass{},quarantined{},abandoned{};
};
struct Queue {Ptr<ID3D12CommandQueue> native;Ptr<ID3D12Fence> fence;std::uint64_t id{},value{},frequency{};};
struct Job {std::shared_ptr<Recording> record;std::shared_ptr<Queue> queue;std::uint64_t value{},serial{},frame{};bool signaled{},sample{};};
struct State {
    std::recursive_mutex mutex;
    std::atomic<bool> capturing{};
    std::atomic<unsigned> tracked{},open{};
    std::shared_ptr<Session> session,completed;
    std::map<ID3D12PipelineState*,std::shared_ptr<Pipeline>> pipelines;
    std::map<ID3D12CommandSignature*,unsigned> signatures;
    std::map<ID3D12GraphicsCommandList*,std::shared_ptr<Recording>> recordings;
    std::map<ID3D12CommandQueue*,std::shared_ptr<Queue>> queues;
    std::array<std::shared_ptr<Recording>,max_records> pool;
    std::array<Job,max_jobs> jobs;
    std::uint64_t next_pipeline{},next_record{},next_queue{},next_session{},faults{};
};
State& state(){static auto* s=new State;return *s;}
template<class F>void safe(F&& fn)noexcept{
    try{std::lock_guard lock(state().mutex);fn();}
    catch(...){auto& s=state();std::lock_guard lock(s.mutex);++s.faults;if(s.session){++s.session->faults;s.session->stopped=true;}s.capturing=false;}
}
constexpr GUID lifetime_guid{0x03c890c1,0x7e4c,0x45b6,{0x80,0x4c,0x2f,0x14,0xe8,0x88,0x8e,0x91}};
void retire_record(ID3D12GraphicsCommandList* native){
    auto& s=state();auto it=s.recordings.find(native);if(it==s.recordings.end())return;
    if(!it->second->closed)--s.open;s.recordings.erase(it);s.tracked=static_cast<unsigned>(s.recordings.size());
}
class Lifetime final:public IUnknown {
    std::atomic<ULONG> refs{1};void* object;int kind;
public:
    Lifetime(void* p,int k):object(p),kind(k){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown)return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{arc::InterceptCpuMeter::Scope cpu_hook(!cpu_cost::on_worker_thread());const auto n=--refs;if(!n){safe([&]{if(kind==1)state().pipelines.erase(static_cast<ID3D12PipelineState*>(object));else if(kind==2)state().signatures.erase(static_cast<ID3D12CommandSignature*>(object));else retire_record(static_cast<ID3D12GraphicsCommandList*>(object));});delete this;}return n;}
};
bool track(ID3D12Object* object,int kind){auto* token=new Lifetime(object,kind);const auto result=object->SetPrivateDataInterface(lifetime_guid,token);token->Release();return SUCCEEDED(result);}
std::string digest(const D3D12_SHADER_BYTECODE& code){
    if(!code.pShaderBytecode||!code.BytecodeLength||code.BytecodeLength>32*1024*1024)return {};
    static BCRYPT_ALG_HANDLE algorithm=[](){BCRYPT_ALG_HANDLE h{};return BCryptOpenAlgorithmProvider(&h,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0?h:nullptr;}();
    unsigned char hash[32]{};if(!algorithm||BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<void*>(code.pShaderBytecode)),static_cast<ULONG>(code.BytecodeLength),hash,32)<0)return {};
    const char* hex="0123456789abcdef";std::string result;result.reserve(64);for(auto b:hash){result+=hex[b>>4];result+=hex[b&15];}return result;
}
class Blob final:public IDxcBlob {
    std::atomic<ULONG> refs{1};D3D12_SHADER_BYTECODE bytes;
public:
    explicit Blob(D3D12_SHADER_BYTECODE b):bytes(b){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown&&iid!=__uuidof(IDxcBlob))return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{arc::InterceptCpuMeter::Scope cpu_hook(!cpu_cost::on_worker_thread());auto n=--refs;if(!n)delete this;return n;}
    LPVOID STDMETHODCALLTYPE GetBufferPointer()override{return const_cast<void*>(bytes.pShaderBytecode);}
    SIZE_T STDMETHODCALLTYPE GetBufferSize()override{return bytes.BytecodeLength;}
};
Shader inspect(const D3D12_SHADER_BYTECODE& code){
    Shader result;result.bytes=code.BytecodeLength;result.hash=digest(code);if(result.hash.empty())return result;
    result.captured=capture_code(code);
    Module module;Ptr<ID3D12ShaderReflection> reflection;
    if(FAILED(D3DReflect(code.pShaderBytecode,code.BytecodeLength,IID_PPV_ARGS(&reflection)))){
        // Use only a compiler already loaded by the application. No implicit
        // DLL search/download and no dependency on an engine installation.
        GetModuleHandleExW(0,L"dxcompiler.dll",&module.handle);auto create=module.handle?reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module.handle,"DxcCreateInstance")):nullptr;
        if(create){Ptr<IDxcContainerReflection> container;Ptr<IDxcBlob> blob;blob.Attach(new Blob(code));UINT part{};
            if(SUCCEEDED(create(CLSID_DxcContainerReflection,IID_PPV_ARGS(&container)))&&SUCCEEDED(container->Load(blob.Get()))&&SUCCEEDED(container->FindFirstPartKind(0x4c495844,&part)))container->GetPartReflection(part,IID_PPV_ARGS(&reflection));}
    }
    if(!reflection)return result;D3D12_SHADER_DESC desc{};if(FAILED(reflection->GetDesc(&desc)))return result;
    result.reflected=true;result.requires_flags=reflection->GetRequiresFlags();reflection->GetThreadGroupSize(&result.threads[0],&result.threads[1],&result.threads[2]);
    result.instructions=desc.InstructionCount;result.barriers=desc.cBarrierInstructions;result.atomics=desc.cInterlockedInstructions;
    result.bindings_truncated=desc.BoundResources>64;
    for(UINT i=0;i<std::min(desc.BoundResources,64u);++i){D3D12_SHADER_INPUT_BIND_DESC b{};if(SUCCEEDED(reflection->GetResourceBindingDesc(i,&b))){result.unbounded|=b.BindCount==0||b.BindCount==UINT_MAX;result.bindings.push_back({static_cast<UINT>(b.Type),b.BindPoint,b.Space,b.BindCount,static_cast<UINT>(b.Dimension)});}}
    return result;
}
std::shared_ptr<Pipeline> lookup(ID3D12PipelineState* pso){auto it=state().pipelines.find(pso);return it==state().pipelines.end()?nullptr:it->second;}
void end_span(ID3D12GraphicsCommandList* native,Recording& r){if(!r.open_span)return;mirror::InternalCall internal;native->EndQuery(r.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,static_cast<UINT>(r.spans.size()*2-1));r.open_span=false;}
void finish(ID3D12GraphicsCommandList* native,Recording& r){
    if(r.closed)return;
    if(r.render_pass){++r.session->unsupported;return;}
    end_span(native,r);mirror::InternalCall internal;
    if(!r.spans.empty())native->ResolveQueryData(r.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,static_cast<UINT>(r.spans.size()*2),r.readback.Get(),0);
    r.closed=true;--state().open;
}
void add_work(ID3D12GraphicsCommandList* native,unsigned kind,UINT x,UINT y,UINT z){
    auto it=state().recordings.find(native);if(it==state().recordings.end())return;auto& r=*it->second;if(r.closed)return;
    auto& session=*r.session;const auto& limits=background_budget_config();if(session.stopped)return;
    if(++session.observed_events>=limits.gpu_capture_events){++session.declined;if(r.session==state().session)stop();return;}
    if((session.observed_events&63)==0&&Clock::now()>=session.deadline){session.timed_out=true;if(r.session==state().session)stop();return;}

    if(r.open_span){const auto& old=r.spans.back();if(old.kind!=kind||(kind==2&&old.pipeline!=r.current))end_span(native,r);}
    if(!r.open_span){
        if(r.spans.size()>=max_spans){++r.session->declined;if(r.session==state().session&&!r.session->stopped)stop();return;}
        if(session.trace_bytes+sizeof(Span)+16>limits.gpu_capture_bytes){++session.declined;if(r.session==state().session)stop();return;}session.trace_bytes+=sizeof(Span)+16;
        Span span;span.kind=kind;span.pipeline=r.current;span.dispatch[0]=x;span.dispatch[1]=y;span.dispatch[2]=z;r.spans.push_back(std::move(span));
        mirror::InternalCall internal;native->EndQuery(r.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,static_cast<UINT>((r.spans.size()-1)*2));r.open_span=true;
    }
    auto& span=r.spans.back();++span.calls;span.items+=std::uint64_t(x)*y*z;
    span.mixed_pipeline|=span.pipeline!=r.current;
    if(kind==2)span.mixed_dispatch|=span.dispatch[0]!=x||span.dispatch[1]!=y||span.dispatch[2]!=z;
}
void shadow_reuse_json(std::ostream& out,const Pipeline& p){
    if(!p.depth_only)return;
    // Depth-only is a candidate class, not proof that the pass is a shadow.
    // Resource identities/reflection do not establish unchanged contents.
    out<<",\"depth_reuse_audit\":{\"semantic_shadow_proven\":false,\"reuse_enabled\":false,\"vertex_reflection_available\":"<<(p.vertex_shader.reflected?"true":"false")
       <<",\"additional_geometry_stages\":"<<(p.extra_geometry_stages?"true":"false")
       <<",\"blockers\":[\"resource_content_versions_untracked\",\"whole_pass_clear_and_draw_replay_unavailable\",\"output_preservation_and_queue_dependencies_unproven\"],\"vertex_declared_bindings\":[";
    bool first=true;for(const auto& b:p.vertex_shader.bindings){if(!first)out<<',';first=false;out<<"{\"type\":"<<b.type<<",\"slot\":"<<b.slot<<",\"space\":"<<b.space<<",\"count\":"<<b.count<<'}';}
    out<<"]}";
}
void shader_json(std::ostream& out,const Pipeline& p){
    const auto& m=p.shader;out<<"{\"id\":"<<p.id<<",\"kind\":\""<<(p.compute?"compute":"graphics")<<"\",\"depth_only\":"<<(p.depth_only?"true":"false")
        <<",\"render_target_count\":"<<p.render_targets<<",\"sha256\":"<<std::quoted(m.hash)<<",\"vertex_sha256\":"<<std::quoted(p.vertex_hash)<<",\"bytecode_bytes\":"<<m.bytes<<",\"reflection_available\":"<<(m.reflected?"true":"false")
        <<",\"threads\":["<<m.threads[0]<<','<<m.threads[1]<<','<<m.threads[2]<<"],\"instructions\":"<<m.instructions<<",\"barrier_instructions\":"<<m.barriers<<",\"atomic_instructions\":"<<m.atomics
        <<",\"requires_flags\":"<<m.requires_flags<<",\"unbounded_bindings\":"<<(m.unbounded?"true":"false")<<",\"bindings_truncated\":"<<(m.bindings_truncated?"true":"false")<<",\"declared_bindings\":[";
    bool first=true;for(const auto& b:m.bindings){if(!first)out<<',';first=false;out<<"{\"type\":"<<b.type<<",\"slot\":"<<b.slot<<",\"space\":"<<b.space<<",\"count\":"<<b.count<<",\"dimension\":"<<b.dimension<<'}';}out<<"]";shadow_reuse_json(out,p);out<<"}";
}
void write_report(Session& session,unsigned pending){
    auto temp=session.path;temp+=L".tmp";std::ofstream out(temp,std::ios::trunc);out<<std::setprecision(14);
    out<<"{\"schema\":1,\"engine_labels_used\":false,\"shader_mutations\":0,\"image_readbacks\":0,\"resource_dependencies_complete\":false,\"present_windows_requested\":"<<session.wanted
        <<",\"present_windows\":"<<session.presents<<",\"timed_out\":"<<(session.timed_out?"true":"false")<<",\"recordings\":"<<session.recorded<<",\"submissions\":"<<session.submitted
        <<",\"observed_events\":"<<session.observed_events<<",\"trace_bytes\":"<<session.trace_bytes<<",\"capacity_declines\":"<<session.declined<<",\"unsupported_segments\":"<<session.unsupported<<",\"cached_replay_samples_dropped\":"<<session.overwritten<<",\"dropped_intervals\":"<<session.dropped
        <<",\"present_thread_id\":"<<session.present_thread<<",\"cpu_begin_100ns\":"<<session.cpu_begin<<",\"cpu_end_100ns\":"<<session.cpu_end<<",\"qpc_begin\":"<<session.qpc_begin<<",\"qpc_end\":"<<session.qpc_end<<",\"mixed_present_sources\":"<<(session.mixed_present?"true":"false")
        <<",\"abandoned_recordings\":"<<session.abandoned<<",\"faults\":"<<session.faults<<",\"pending_gpu_jobs\":"<<pending<<",\"intervals\":[";
    std::map<std::uint64_t,std::shared_ptr<Pipeline>> pipelines;bool first=true;
    for(const auto& row:session.rows){if(!first)out<<',';first=false;const auto& span=row.span;if(span.pipeline)pipelines[span.pipeline->id]=span.pipeline;
        out<<"{\"recording\":"<<row.recording<<",\"frame\":"<<row.frame<<",\"queue\":"<<row.queue<<",\"submission\":"<<row.submission<<",\"kind\":\""<<std::array{"raster_color","raster_depth","compute","opaque","raster_unknown"}[span.kind]
            <<"\",\"pipeline\":"<<(span.pipeline?span.pipeline->id:0)<<",\"mixed_pipeline\":"<<(span.mixed_pipeline?"true":"false")<<",\"calls\":"<<span.calls<<",\"items\":"<<span.items
            <<",\"indirect_calls\":"<<span.indirect_calls<<",\"indirect_max_commands\":"<<span.indirect_max_commands<<",\"dispatch\":["<<span.dispatch[0]<<','<<span.dispatch[1]<<','<<span.dispatch[2]<<"],\"mixed_dispatch\":"<<(span.mixed_dispatch?"true":"false")
            <<",\"begin_ticks\":"<<row.start<<",\"end_ticks\":"<<row.end<<",\"frequency\":"<<row.frequency<<",\"gpu_ms\":"<<double(row.end-row.start)*1000.0/double(row.frequency)<<'}';
    }
    out<<"],\"pipelines\":[";first=true;for(const auto& [id,p]:pipelines){if(!first)out<<',';first=false;shader_json(out,*p);}out<<"]}\n";out.close();
    bool directory_created=false;auto code_directory=session.path;code_directory+=L".shaders";
    std::set<std::string> exported;
    for(const auto& [id,p]:pipelines){
        (void)id;const auto& shader=p->shader;
        if(!shader.captured||!exported.insert(shader.hash).second)continue;
        if(!directory_created){if(!std::filesystem::create_directory(code_directory))throw std::runtime_error("Shader capture directory exists");directory_created=true;}
        std::ofstream binary(code_directory/(shader.hash+".bin"),std::ios::binary);
        binary.write(reinterpret_cast<const char*>(shader.captured->bytes.data()),static_cast<std::streamsize>(shader.captured->bytes.size()));binary.close();
        if(!binary)throw std::runtime_error("Shader capture publication failed");
    }
    if(!out||!MoveFileExW(temp.c_str(),session.path.c_str(),0))throw std::runtime_error("GPU profile publication failed");session.written=true;
}
}

void graphics_created(ID3D12PipelineState* pso,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc)noexcept{
    if(mirror::internal()||!pso||!desc)return;
    safe([&]{auto& s=state();if(s.pipelines.contains(pso)||s.pipelines.size()>=16384)return;mirror::InternalCall internal;auto p=std::make_shared<Pipeline>();p->shader=inspect(desc->PS);p->vertex_hash=digest(desc->VS);p->render_targets=desc->NumRenderTargets;p->depth_only=desc->NumRenderTargets==0&&(desc->DepthStencilState.DepthEnable||desc->DepthStencilState.StencilEnable);if(p->depth_only){p->vertex_shader=inspect(desc->VS);p->extra_geometry_stages=desc->HS.BytecodeLength||desc->DS.BytecodeLength||desc->GS.BytecodeLength;}
        if(!track(pso,true))return;p->id=++s.next_pipeline;s.pipelines[pso]=std::move(p);});
}
void compute_created(ID3D12PipelineState* pso,const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc)noexcept{
    if(mirror::internal()||!pso||!desc)return;
    safe([&]{auto& s=state();if(s.pipelines.contains(pso)||s.pipelines.size()>=16384)return;mirror::InternalCall internal;auto p=std::make_shared<Pipeline>();p->compute=true;p->shader=inspect(desc->CS);
        if(!track(pso,true))return;p->id=++s.next_pipeline;s.pipelines[pso]=std::move(p);});
}
void stream_created(ID3D12PipelineState* pso,const D3D12_PIPELINE_STATE_STREAM_DESC* stream)noexcept{
    if(mirror::internal()||!pso||!stream||!stream->pPipelineStateSubobjectStream||stream->SizeInBytes>65536)return;
    safe([&]{
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};D3D12_SHADER_BYTECODE compute{};std::size_t offset=0;bool samples=false,raster=false;
        std::set<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE> seen;
        const auto* bytes=static_cast<const unsigned char*>(stream->pPipelineStateSubobjectStream);
        auto read=[&]<class T>(T* output){struct alignas(void*) Item{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;T value;};
            if(sizeof(Item)>stream->SizeInBytes-offset)return false;Item value;std::memcpy(&value,bytes+offset,sizeof(value));if(output)*output=value.value;offset+=sizeof(Item);return true;};
        while(offset<stream->SizeInBytes){
            if(stream->SizeInBytes-offset<sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))return;
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;std::memcpy(&type,bytes+offset,sizeof(type));if(!seen.insert(type).second)return;
            bool ok=false;
            switch(type){
#define SKIP_SUBOBJECT(name,T) case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##name:ok=read(static_cast<T*>(nullptr));break
            SKIP_SUBOBJECT(ROOT_SIGNATURE,ID3D12RootSignature*);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:ok=read(&desc.VS);break;SKIP_SUBOBJECT(DS,D3D12_SHADER_BYTECODE);SKIP_SUBOBJECT(HS,D3D12_SHADER_BYTECODE);SKIP_SUBOBJECT(GS,D3D12_SHADER_BYTECODE);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:ok=read(&desc.PS);break;
            // Compute/mesh state cannot authorize a raster draw override.
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:ok=read(&compute);break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:return;
            SKIP_SUBOBJECT(STREAM_OUTPUT,D3D12_STREAM_OUTPUT_DESC);SKIP_SUBOBJECT(BLEND,D3D12_BLEND_DESC);SKIP_SUBOBJECT(SAMPLE_MASK,UINT);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:ok=read(&desc.RasterizerState);raster=ok;break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:{D3D12_RASTERIZER_DESC1 r{};ok=read(&r);desc.RasterizerState.ForcedSampleCount=r.ForcedSampleCount;raster=ok;break;}
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:{D3D12_RASTERIZER_DESC2 r{};ok=read(&r);desc.RasterizerState.ForcedSampleCount=r.ForcedSampleCount;raster=ok;break;}
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:ok=read(&desc.DepthStencilState);break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:{D3D12_DEPTH_STENCIL_DESC1 d{};ok=read(&d);desc.DepthStencilState.DepthEnable=d.DepthEnable;desc.DepthStencilState.StencilEnable=d.StencilEnable;break;}
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:{D3D12_DEPTH_STENCIL_DESC2 d{};ok=read(&d);desc.DepthStencilState.DepthEnable=d.DepthEnable;desc.DepthStencilState.StencilEnable=d.StencilEnable;break;}
            SKIP_SUBOBJECT(INPUT_LAYOUT,D3D12_INPUT_LAYOUT_DESC);SKIP_SUBOBJECT(IB_STRIP_CUT_VALUE,D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
            SKIP_SUBOBJECT(PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE);case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:{D3D12_RT_FORMAT_ARRAY formats{};ok=read(&formats);desc.NumRenderTargets=formats.NumRenderTargets;break;}SKIP_SUBOBJECT(DEPTH_STENCIL_FORMAT,DXGI_FORMAT);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:ok=read(&desc.SampleDesc);samples=ok;break;
            SKIP_SUBOBJECT(NODE_MASK,UINT);SKIP_SUBOBJECT(CACHED_PSO,D3D12_CACHED_PIPELINE_STATE);SKIP_SUBOBJECT(FLAGS,D3D12_PIPELINE_STATE_FLAGS);SKIP_SUBOBJECT(VIEW_INSTANCING,D3D12_VIEW_INSTANCING_DESC);
#undef SKIP_SUBOBJECT
            default:return;
            }if(!ok)return;
        }
        if(compute.pShaderBytecode){D3D12_COMPUTE_PIPELINE_STATE_DESC cs{};cs.CS=compute;compute_created(pso,&cs);}
        else if(desc.VS.pShaderBytecode||desc.PS.pShaderBytecode)graphics_created(pso,&desc);
    });
}

void signature_created(ID3D12CommandSignature* signature,const D3D12_COMMAND_SIGNATURE_DESC* desc)noexcept{
    if(mirror::internal()||!signature||!desc)return;
    safe([&]{auto& s=state();if(s.signatures.contains(signature)||s.signatures.size()>=16384)return;bool raster=false,compute=false,other=false;
        for(UINT i=0;i<desc->NumArgumentDescs;++i){const auto type=desc->pArgumentDescs[i].Type;raster|=type==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW||type==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;compute|=type==D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;other|=type==D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS||type==D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;}
        mirror::InternalCall internal;if(track(signature,2))s.signatures[signature]=other||(raster==compute)?3:compute?2:0;
    });
}
void indirect(ID3D12GraphicsCommandList* native,ID3D12CommandSignature* signature,UINT max_commands)noexcept{
    if(mirror::internal()||!state().open)return;
    safe([&]{auto& s=state();auto r=s.recordings.find(native);if(r==s.recordings.end()||r->second->closed)return;auto kind=s.signatures.find(signature);
        if(kind==s.signatures.end()||kind->second==3){opaque(native);return;}
        const auto count=r->second->spans.size();const bool open=r->second->open_span;auto pipeline=r->second->current;
        add_work(native,kind->second==2?2:!pipeline?4:pipeline->depth_only?1:pipeline->render_targets?0:4,0,0,0);
        if(r->second->open_span&&(open||r->second->spans.size()>count)){auto& span=r->second->spans.back();++span.indirect_calls;span.indirect_max_commands+=max_commands;}
    });
}
void begin(ID3D12GraphicsCommandList* native,ID3D12PipelineState* pso)noexcept{
    auto& s=state();if(mirror::internal()||(!s.capturing.load(std::memory_order_relaxed)&&!s.tracked.load(std::memory_order_relaxed)))return;
    safe([&]{retire_record(native);if(!s.capturing||!s.session||s.session->stopped)return;const auto type=native->GetType();if(type!=D3D12_COMMAND_LIST_TYPE_DIRECT&&type!=D3D12_COMMAND_LIST_TYPE_COMPUTE){++s.session->declined;return;}
        mirror::InternalCall internal;Ptr<ID3D12Device> device;if(FAILED(native->GetDevice(IID_PPV_ARGS(&device)))){++s.session->faults;return;}
        if(device->GetNodeCount()!=1){++s.session->declined;return;} // Linked-adapter node visibility is not established.
        auto free=std::find_if(s.pool.begin(),s.pool.end(),[&](const auto& r){return r&&r.use_count()==1&&!r->quarantined&&r->device.Get()==device.Get();});
        if(free==s.pool.end())free=std::find(s.pool.begin(),s.pool.end(),nullptr);
        if(free==s.pool.end()){++s.session->declined;return;}
        auto r=*free?*free:std::make_shared<Recording>();r->device=device;r->session=s.session;r->id=++s.next_record;r->frame=s.session->presents;r->current=lookup(pso);
        r->spans.clear();r->spans.reserve(max_spans);r->serial=0;r->open_span=r->closed=r->render_pass=r->quarantined=r->abandoned=false;
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=max_spans*2;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=max_spans*2*8;d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if((!r->queries&&FAILED(r->device->CreateQueryHeap(&q,IID_PPV_ARGS(&r->queries))))||(!r->readback&&FAILED(r->device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r->readback))))||!track(native,false)){++s.session->faults;return;}
        *free=r;s.recordings[native]=r;++s.open;s.tracked=static_cast<unsigned>(s.recordings.size());++s.session->recorded;
    });
}
void pipeline(ID3D12GraphicsCommandList* native,ID3D12PipelineState* pso)noexcept{
    if(mirror::internal()||!state().open.load(std::memory_order_relaxed))return;
    safe([&]{auto it=state().recordings.find(native);if(it==state().recordings.end()||it->second->closed)return;auto& r=*it->second;auto next=lookup(pso);
        if(r.open_span){const auto kind=r.spans.back().kind;if(kind>=2||(next&&next->compute)||(next&&unsigned(next->depth_only)!=kind))end_span(native,r);}r.current=std::move(next);
    });
}
void work(ID3D12GraphicsCommandList* native,bool compute,UINT x,UINT y,UINT z)noexcept{
    if(mirror::internal()||!state().open.load(std::memory_order_relaxed))return;
    safe([&]{auto it=state().recordings.find(native);if(it==state().recordings.end())return;const auto& p=it->second->current;add_work(native,compute?2:!p?4:p->depth_only?1:p->render_targets?0:4,x,y,z);});
}
void opaque(ID3D12GraphicsCommandList* native)noexcept{
    if(mirror::internal()||!state().open.load(std::memory_order_relaxed))return;
    safe([&]{auto it=state().recordings.find(native);if(it==state().recordings.end()||it->second->closed)return;++it->second->session->unsupported;it->second->current.reset();add_work(native,3,1,1,1);});
}
void render_pass(ID3D12GraphicsCommandList* native,bool beginning,D3D12_RENDER_PASS_FLAGS flags)noexcept{
    if(mirror::internal()||!state().open.load(std::memory_order_relaxed))return;
    safe([&]{auto it=state().recordings.find(native);if(it==state().recordings.end()||it->second->closed)return;auto& r=*it->second;
        if(beginning&&(flags&(D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS|D3D12_RENDER_PASS_FLAG_RESUMING_PASS))){++r.session->unsupported;finish(native,r);return;}r.render_pass=beginning;});
}
void protected_session(ID3D12GraphicsCommandList* native)noexcept{if(mirror::internal()||!state().open)return;safe([&]{auto it=state().recordings.find(native);if(it!=state().recordings.end()){++it->second->session->unsupported;finish(native,*it->second);}});}
void close(ID3D12GraphicsCommandList* native)noexcept{if(mirror::internal()||!state().open)return;safe([&]{auto it=state().recordings.find(native);if(it!=state().recordings.end())finish(native,*it->second);});}

Submission before_submit(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* native)noexcept{
    Submission result;auto& s=state();if(mirror::internal()||!s.tracked||!native)return result;
    result.lock=std::unique_lock(s.mutex);
    try{mirror::InternalCall internal;std::shared_ptr<Queue> q;
        for(UINT i=0;i<count;++i){auto it=s.recordings.find(reinterpret_cast<ID3D12GraphicsCommandList*>(native[i]));if(it==s.recordings.end())continue;auto r=it->second;++r->serial;
            if(!r->closed||r->spans.empty())continue;
            if(!q){auto known=s.queues.find(queue);if(known!=s.queues.end())q=known->second;else if(s.queues.size()<16){q=std::make_shared<Queue>();q->native=queue;q->id=++s.next_queue;
                if(FAILED(queue->GetTimestampFrequency(&q->frequency))||!q->frequency||FAILED(r->device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&q->fence))))q.reset();else s.queues[queue]=q;}}
            auto free=std::find_if(s.jobs.begin(),s.jobs.end(),[](const Job& job){return !job.record;});
            if(!q||free==s.jobs.end()){r->quarantined=true;++r->session->declined;continue;}
            *free={r,q,0,r->serial,r->session->presents,false,!r->abandoned&&!r->session->stopped&&!r->session->written&&!r->session->publishing};result.jobs[result.count++]=static_cast<unsigned short>(free-s.jobs.begin());if(free->sample)++r->session->submitted;
        }
    }catch(...){++s.faults;for(UINT i=0;i<count;++i){auto it=s.recordings.find(reinterpret_cast<ID3D12GraphicsCommandList*>(native[i]));if(it!=s.recordings.end()){it->second->quarantined=true;++it->second->session->faults;}}}
    return result;
}
void after_submit(Submission& submission,ID3D12CommandQueue* native)noexcept{
    if(!submission.count)return;auto& s=state();mirror::InternalCall internal;auto q=s.jobs[submission.jobs[0]].queue;const auto value=++q->value;const bool signaled=SUCCEEDED(native->Signal(q->fence.Get(),value));
    for(UINT i=0;i<submission.count;++i){auto& job=s.jobs[submission.jobs[i]];job.value=value;job.signaled=signaled;if(!signaled){job.record->quarantined=true;++job.record->session->faults;}}
}
bool request(const std::wstring& path,UINT windows)noexcept{
    if(!hooks::begin_raster_observation())return false;
    struct EndSetup {~EndSetup(){hooks::end_raster_observation();}} setup;
    bool accepted=false;safe([&]{auto& s=state();if(path.empty()||windows<1||windows>128||(s.session&&!s.session->written&&!s.session->export_failed)||!std::filesystem::is_directory(std::filesystem::absolute(path).parent_path())||std::filesystem::exists(path)||std::filesystem::exists(path+L".tmp"))return;
        auto session=std::make_shared<Session>();session->id=++s.next_session;session->path=path;session->wanted=windows;session->deadline=Clock::now()+background_budget_config().gpu_capture_window;session->drain_deadline=session->deadline+std::chrono::seconds(5);session->rows.reserve(max_rows);s.session=std::move(session);s.capturing=true;accepted=true;});return accepted;
}
bool busy()noexcept{bool result=true;safe([&]{auto& s=state();result=s.open||s.capturing||(s.session&&!s.session->written&&!s.session->export_failed);});return result;}
bool needs_raster_observation()noexcept{return state().open.load(std::memory_order_relaxed)||state().capturing.load(std::memory_order_relaxed);}
void stop()noexcept{safe([&]{auto& s=state();s.capturing=false;if(s.session&&!s.session->stopped){s.session->stopped=true;s.session->drain_deadline=Clock::now()+std::chrono::seconds(5);}});}
void present(void* swapchain)noexcept{if(!state().capturing)return;safe([&]{auto& s=state();if(!s.session||s.session->stopped)return;auto& session=*s.session;
    FILETIME created{},exited{},kernel{},user{};LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    const auto bits=[](FILETIME v){return (std::uint64_t(v.dwHighDateTime)<<32)|v.dwLowDateTime;};
    const bool valid=GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)!=FALSE;
    const auto cpu=valid?bits(kernel)+bits(user):0;session.cpu_failed|=!valid;
    if(!session.presents){session.cpu_begin=cpu;session.qpc_begin=now.QuadPart;session.present_thread=GetCurrentThreadId();session.swapchain=swapchain;}
    else session.mixed_present|=session.present_thread!=GetCurrentThreadId()||session.swapchain!=swapchain;
    session.cpu_end=cpu;session.qpc_end=now.QuadPart;
    if(++session.presents>=session.wanted)stop();
});}
void collect()noexcept{
    std::shared_ptr<Session> publication,owner;unsigned publication_pending=0;
    safe([&]{auto& s=state();mirror::InternalCall internal;
        if(s.capturing&&s.session&&Clock::now()>=s.session->deadline){s.session->timed_out=true;stop();}
        for(auto& job:s.jobs){if(!job.record)continue;auto& r=*job.record;auto& session=*r.session;
            if(FAILED(r.device->GetDeviceRemovedReason())){++session.faults;job={};continue;}
            if(!job.signaled||job.queue->fence->GetCompletedValue()<job.value)continue;
            if(job.sample&&!session.written&&!session.publishing&&!session.export_failed){
                if(job.serial!=r.serial)++session.overwritten;
                else{void* data{};D3D12_RANGE range{0,r.spans.size()*16},empty{};
                    if(SUCCEEDED(r.readback->Map(0,&range,&data))){const auto* ticks=static_cast<const UINT64*>(data);
                        for(std::size_t i=0;i<r.spans.size();++i){if(session.rows.size()>=max_rows||session.trace_bytes+sizeof(Row)>background_budget_config().gpu_capture_bytes){++session.dropped;continue;}session.trace_bytes+=sizeof(Row);if(!ticks[i*2]||!ticks[i*2+1]||ticks[i*2+1]<ticks[i*2]||double(ticks[i*2+1]-ticks[i*2])/double(job.queue->frequency)>10){++session.faults;continue;}
                            session.rows.push_back({r.spans[i],r.id,job.frame,job.queue->id,job.serial,ticks[i*2],ticks[i*2+1],job.queue->frequency});}
                        r.readback->Unmap(0,&empty);
                    }else ++session.faults;
                }
            }job={};
        }
        // An application may keep an open command list indefinitely. After
        // the bounded drain, abandon measurement, not the application's list.
        // Keep query storage owned by the recording and submission fence jobs.
        for(auto& [native,r]:s.recordings)if(!r->closed&&r->session->stopped&&Clock::now()>=r->session->drain_deadline){
            (void)native;r->abandoned=true;r->closed=true;r->open_span=false;--s.open;++r->session->abandoned;
        }
        for(auto& r:s.pool)if(r&&r.use_count()==1&&!s.capturing&&(!r->quarantined||FAILED(r->device->GetDeviceRemovedReason())))r.reset();
        if(!s.capturing&&!s.tracked){for(auto it=s.queues.begin();it!=s.queues.end();){if(it->second.use_count()==1)it=s.queues.erase(it);else ++it;}}
        if(s.session&&s.session->stopped&&!s.session->written&&!s.session->publishing&&!s.session->export_failed){unsigned pending=0;for(const auto& j:s.jobs)if(j.record&&j.record->session==s.session)++pending;
            for(const auto& [native,r]:s.recordings)if(r->session==s.session&&!r->closed&&!r->spans.empty())++pending;
            if(!pending||Clock::now()>=s.session->drain_deadline){publication=std::make_shared<Session>(*s.session);owner=s.session;publication_pending=pending;s.session->publishing=true;}}
    });
    if(publication){try{write_report(*publication,publication_pending);safe([&]{owner->written=true;owner->publishing=false;owner->completed_tick=GetTickCount64();owner->pending_publication=publication_pending!=0;state().completed=owner;});}catch(...){safe([&]{owner->publishing=false;owner->export_failed=true;++owner->faults;++state().faults;});}}
}
void snapshot(std::ostream& out){auto& s=state();std::lock_guard lock(s.mutex);out<<"{\"capturing\":"<<(s.capturing?"true":"false")<<",\"export_failed\":"<<(s.session&&s.session->export_failed?"true":"false")<<",\"live_pipelines\":"<<s.pipelines.size()<<",\"tracked_recordings\":"<<s.tracked.load()<<",\"retained_gpu_recordings\":"<<std::count_if(s.pool.begin(),s.pool.end(),[](const auto& r){return bool(r);})<<",\"faults\":"<<s.faults<<'}';}
std::vector<ComputeCost> compute_costs()noexcept{
    std::vector<ComputeCost> result;
    safe([&]{const auto& s=state();const auto& session=s.completed;if(!session||session->faults||session->rows.empty())return;
        std::map<const Pipeline*,double> costs;
        for(const auto& row:session->rows)if(row.span.kind==2&&row.span.pipeline&&!row.span.mixed_pipeline)
            costs[row.span.pipeline.get()]+=double(row.end-row.start)*1000.0/double(row.frequency);
        for(const auto& [native,pipeline]:s.pipelines)if(auto it=costs.find(pipeline.get());it!=costs.end())result.push_back({native,session->id,pipeline->id,it->second,session->presents});
    });return result;
}
LoadEvidence load_evidence()noexcept{
    LoadEvidence out;safe([&]{const auto& s=state();if(!s.completed)return;const auto& p=*s.completed;out.sequence=p.id;out.completed_tick_ms=p.completed_tick;
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);if(p.presents<2||p.qpc_end<=p.qpc_begin)return;
        const double windows=p.presents-1,wall=double(p.qpc_end-p.qpc_begin)*1000/frequency.QuadPart;out.frame_ms=wall/windows;
        const bool healthy=!p.faults&&!p.dropped&&!p.overwritten&&!p.timed_out&&!p.pending_publication&&!p.mixed_present&&!p.abandoned&&!p.declined&&!p.unsupported;
        out.cpu_valid=healthy&&!p.cpu_failed&&wall>=200&&p.cpu_end>=p.cpu_begin;
        if(out.cpu_valid)out.cpu_running_ms=double(p.cpu_end-p.cpu_begin)/10000/windows;
        std::map<std::uint64_t,std::vector<std::pair<std::uint64_t,std::uint64_t>>> intervals;std::map<std::uint64_t,std::uint64_t> frequencies;
        for(const auto& r:p.rows)if(r.frame>0&&r.frame<p.presents&&r.frequency&&r.end>=r.start){intervals[r.queue].push_back({r.start,r.end});frequencies[r.queue]=r.frequency;}
        for(auto& [queue,spans]:intervals){const auto total=arc::queue_union_ticks(std::move(spans));
            out.gpu_queue_busy_ms=std::max(out.gpu_queue_busy_ms,double(total)*1000/frequencies[queue]/windows);}
        // This is a lower bound from the busiest observed queue, not a sum of
        // possibly overlapping queues or a claim of complete GPU frame timing.
        out.gpu_valid=healthy&&!intervals.empty();
    });return out;
}
std::uint64_t pipeline_identity(ID3D12PipelineState* native)noexcept{std::uint64_t id{};safe([&]{if(auto p=lookup(native))id=p->id;});return id;}
}
