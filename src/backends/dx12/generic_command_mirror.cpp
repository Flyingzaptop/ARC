#include "generic_command_mirror.hpp"
#include <wrl/client.h>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <ostream>
#include <cstring>
#include <set>
#include <chrono>

namespace arc::dx12::mirror {
namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
thread_local bool nested{};
struct Internal {bool old{nested};Internal(){nested=true;}~Internal(){nested=old;}};
struct Command {
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> list;
    Ptr<ID3D12GraphicsCommandList5> v5;
    UINT rate{D3D12_SHADING_RATE_1X1};
    D3D12_SHADING_RATE_COMBINER combiners[2]{};
    std::uint64_t epoch{},events{},modified{},potential{};
    bool eligible_pipeline{},closed{},valid{true};
};
struct Job {std::vector<std::shared_ptr<Command>> commands;Ptr<ID3D12Fence> fence;Ptr<ID3D12Device> device;bool signaled{};};
struct State {
    std::recursive_mutex mutex;
    std::atomic<UINT> rate{};
    std::chrono::steady_clock::time_point expires;
    std::uint64_t epoch{},modified_submissions{},modified_draws{},skipped{},faults{};
    std::map<ID3D12GraphicsCommandList*,std::shared_ptr<Command>> commands;
    std::map<ID3D12PipelineState*,bool> pipelines;
    std::map<ID3D12CommandSignature*,bool> signatures;
    std::set<ID3D12GraphicsCommandList*> command_lifetimes;
    std::vector<Job> jobs;
};
State& state(){static auto* s=new State;return *s;}
constexpr GUID lifetime_guid{0x614ed930,0x83d7,0x44ca,{0xb9,0x06,0x35,0x9e,0x65,0x1d,0x3b,0x1a}};
class Lifetime final:public IUnknown {
    std::atomic<ULONG> count{1};void* object;int kind;
public:
    Lifetime(void* p,int k):object(p),kind(k){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown)return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++count;}
    ULONG STDMETHODCALLTYPE Release()override{const auto n=--count;if(!n){auto& s=state();{std::lock_guard lock(s.mutex);if(kind==1){s.commands.erase(static_cast<ID3D12GraphicsCommandList*>(object));s.command_lifetimes.erase(static_cast<ID3D12GraphicsCommandList*>(object));}else if(kind==2)s.signatures.erase(static_cast<ID3D12CommandSignature*>(object));else s.pipelines.erase(static_cast<ID3D12PipelineState*>(object));}delete this;}return n;}
};
bool track(ID3D12Object* object,int kind){auto* token=new Lifetime(object,kind);const auto hr=object->SetPrivateDataInterface(lifetime_guid,token);token->Release();return SUCCEEDED(hr);}
template<class F>void safe(F&& fn)noexcept{try{std::lock_guard lock(state().mutex);fn();}catch(...){state().rate=0;}}
}
bool internal()noexcept{return nested;}
UINT requested_rate()noexcept{return state().rate.load();}
std::uint64_t modified_draws()noexcept{std::lock_guard lock(state().mutex);return state().modified_draws;}
InternalCall::InternalCall()noexcept:old(nested){nested=true;}
InternalCall::~InternalCall(){nested=old;}
bool configure(UINT rate)noexcept{
    if(rate!=0&&rate!=D3D12_SHADING_RATE_2X2)return false;
    safe([&]{auto& s=state();++s.epoch;s.rate=rate;s.expires=std::chrono::steady_clock::now()+std::chrono::seconds(60);s.commands.clear();});return true;
}
void begin(ID3D12GraphicsCommandList* native,ID3D12PipelineState* pso)noexcept{
    if(nested||!state().rate.load(std::memory_order_relaxed))return;
    safe([&]{auto& s=state();std::shared_ptr<Command> reusable;bool build=true;auto previous=s.commands.find(native);
        if(previous!=s.commands.end()){
            build=previous->second->valid&&previous->second->potential>0;
            if(build&&previous->second.use_count()==1&&previous->second->closed&&previous->second->list)reusable=std::move(previous->second);
            s.commands.erase(previous);
        }
        if(!s.rate||native->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
        if(s.commands.size()>=128||s.jobs.size()>=32){++s.skipped;return;}
        Internal guard;auto c=reusable?std::move(reusable):std::make_shared<Command>();
        c->epoch=s.epoch;c->events=c->modified=c->potential=0;c->closed=false;c->valid=true;
        c->rate=D3D12_SHADING_RATE_1X1;c->combiners[0]=c->combiners[1]=D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
        if(build){
            if(c->allocator){if(FAILED(c->allocator->Reset())||FAILED(c->list->Reset(c->allocator.Get(),pso)))return;}
            else{
                Ptr<ID3D12Device> device;if(FAILED(native->GetDevice(IID_PPV_ARGS(&device))))return;
                D3D12_FEATURE_DATA_D3D12_OPTIONS6 caps{};
                if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,&caps,sizeof(caps)))||caps.VariableShadingRateTier<D3D12_VARIABLE_SHADING_RATE_TIER_1)return;
                if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&c->allocator)))||
                   FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,c->allocator.Get(),pso,IID_PPV_ARGS(&c->list)))||FAILED(c->list.As(&c->v5)))return;
            }
        }
        c->eligible_pipeline=pso&&s.pipelines.contains(pso)&&s.pipelines.at(pso);
        // Unsupported or non-raster prior recordings get a metadata-only probe.
        // If their contents change, a later reset can become a candidate again.
        if(!s.command_lifetimes.contains(native)){if(s.command_lifetimes.size()>=16384||!track(native,1))return;s.command_lifetimes.insert(native);}s.commands[native]=std::move(c);
    });
}
Lease acquire(ID3D12GraphicsCommandList* native)noexcept{
    Lease lease;if(nested||!state().rate.load(std::memory_order_relaxed))return lease;
    safe([&]{auto& s=state();auto it=s.commands.find(native);if(it==s.commands.end())return;auto& c=it->second;
        if(!c->valid||c->closed||c->epoch!=s.epoch)return;if(++c->events>16384){c->valid=false;++s.skipped;return;}if(c->list){lease.owner=c;lease.list=c->list.Get();}});return lease;
}
Lease acquire_draw(ID3D12GraphicsCommandList* native)noexcept{
    if(nested||!state().rate)return {};
    safe([&]{auto it=state().commands.find(native);if(it!=state().commands.end()&&it->second->eligible_pipeline)++it->second->potential;});
    return acquire(native);
}
void close(ID3D12GraphicsCommandList* native)noexcept{if(nested||!state().rate)return;safe([&]{auto it=state().commands.find(native);if(it==state().commands.end())return;Internal guard;auto& c=*it->second;c.closed=true;if(c.list&&FAILED(c.list->Close()))c.valid=false;});}
void invalidate(ID3D12GraphicsCommandList* native)noexcept{if(nested||!state().rate)return;safe([&]{auto it=state().commands.find(native);if(it!=state().commands.end())it->second->valid=false;});}
void pipeline_created(ID3D12PipelineState* pso,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc)noexcept{
    if(nested||!pso||!desc)return;safe([&]{auto& s=state();if(s.pipelines.size()>=16384)return;if(!track(pso,false))return;
        s.pipelines[pso]=desc->PS.pShaderBytecode&&desc->PS.BytecodeLength&&desc->SampleDesc.Count>=1&&desc->SampleDesc.Count<=4&&desc->RasterizerState.ForcedSampleCount==0;});
}
void signature_created(ID3D12CommandSignature* signature,const D3D12_COMMAND_SIGNATURE_DESC* desc)noexcept{
    if(nested||!signature||!desc)return;safe([&]{auto& s=state();if(s.signatures.size()>=16384||!track(signature,2))return;
        bool raster=false,other=false;for(UINT i=0;i<desc->NumArgumentDescs;++i){const auto type=desc->pArgumentDescs[i].Type;
            if(type==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW||type==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)raster=true;
            if(type==D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH||type==D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS||type==D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH)other=true;
        }s.signatures[signature]=raster&&!other;});
}
bool raster_indirect(ID3D12CommandSignature* signature)noexcept{bool raster=false;safe([&]{const auto it=state().signatures.find(signature);raster=it!=state().signatures.end()&&it->second;});return raster;}
void pipeline_stream_created(ID3D12PipelineState* pso,const D3D12_PIPELINE_STATE_STREAM_DESC* stream)noexcept{
    if(nested||!pso||!stream||!stream->pPipelineStateSubobjectStream||stream->SizeInBytes>65536)return;
    safe([&]{
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};std::size_t offset=0;bool samples=false,raster=false;
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
            SKIP_SUBOBJECT(VS,D3D12_SHADER_BYTECODE);SKIP_SUBOBJECT(DS,D3D12_SHADER_BYTECODE);SKIP_SUBOBJECT(HS,D3D12_SHADER_BYTECODE);SKIP_SUBOBJECT(GS,D3D12_SHADER_BYTECODE);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:ok=read(&desc.PS);break;
            // Compute/mesh state cannot authorize a raster draw override.
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:return;
            SKIP_SUBOBJECT(STREAM_OUTPUT,D3D12_STREAM_OUTPUT_DESC);SKIP_SUBOBJECT(BLEND,D3D12_BLEND_DESC);SKIP_SUBOBJECT(SAMPLE_MASK,UINT);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:ok=read(&desc.RasterizerState);raster=ok;break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:{D3D12_RASTERIZER_DESC1 r{};ok=read(&r);desc.RasterizerState.ForcedSampleCount=r.ForcedSampleCount;raster=ok;break;}
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:{D3D12_RASTERIZER_DESC2 r{};ok=read(&r);desc.RasterizerState.ForcedSampleCount=r.ForcedSampleCount;raster=ok;break;}
            SKIP_SUBOBJECT(DEPTH_STENCIL,D3D12_DEPTH_STENCIL_DESC);SKIP_SUBOBJECT(DEPTH_STENCIL1,D3D12_DEPTH_STENCIL_DESC1);SKIP_SUBOBJECT(DEPTH_STENCIL2,D3D12_DEPTH_STENCIL_DESC2);
            SKIP_SUBOBJECT(INPUT_LAYOUT,D3D12_INPUT_LAYOUT_DESC);SKIP_SUBOBJECT(IB_STRIP_CUT_VALUE,D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
            SKIP_SUBOBJECT(PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE);SKIP_SUBOBJECT(RENDER_TARGET_FORMATS,D3D12_RT_FORMAT_ARRAY);SKIP_SUBOBJECT(DEPTH_STENCIL_FORMAT,DXGI_FORMAT);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:ok=read(&desc.SampleDesc);samples=ok;break;
            SKIP_SUBOBJECT(NODE_MASK,UINT);SKIP_SUBOBJECT(CACHED_PSO,D3D12_CACHED_PIPELINE_STATE);SKIP_SUBOBJECT(FLAGS,D3D12_PIPELINE_STATE_FLAGS);SKIP_SUBOBJECT(VIEW_INSTANCING,D3D12_VIEW_INSTANCING_DESC);
#undef SKIP_SUBOBJECT
            default:return;
            }if(!ok)return;
        }
        if(samples&&raster)pipeline_created(pso,&desc);
    });
}
void pipeline(ID3D12GraphicsCommandList* native,ID3D12PipelineState* pso)noexcept{
    if(nested||!state().rate)return;safe([&]{auto& s=state();auto it=s.commands.find(native);if(it!=s.commands.end())it->second->eligible_pipeline=pso&&s.pipelines.contains(pso)&&s.pipelines.at(pso);});
}
void shading_rate(ID3D12GraphicsCommandList* native,D3D12_SHADING_RATE rate,const D3D12_SHADING_RATE_COMBINER* combiners)noexcept{
    if(nested||!state().rate)return;safe([&]{auto it=state().commands.find(native);if(it==state().commands.end())return;auto& c=*it->second;c.rate=rate;for(int i=0;i<2;++i)c.combiners[i]=combiners?combiners[i]:D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;});
}
bool before_draw(const Lease& lease)noexcept{
    if(!lease.owner)return false;auto c=std::static_pointer_cast<Command>(lease.owner);
    if(!c->eligible_pipeline)return false;Internal guard;D3D12_SHADING_RATE_COMBINER pass[2]{};
    c->v5->RSSetShadingRate(D3D12_SHADING_RATE_2X2,pass);++c->modified;return true;
}
void after_draw(const Lease& lease)noexcept{if(!lease.owner)return;auto c=std::static_pointer_cast<Command>(lease.owner);Internal guard;c->v5->RSSetShadingRate(static_cast<D3D12_SHADING_RATE>(c->rate),c->combiners);}
bool execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* original)noexcept{
    if(nested||!state().rate||!count||count>1024)return false;bool replaced=false;
    safe([&]{auto& s=state();if(std::chrono::steady_clock::now()>=s.expires){configure(0);return;}if(s.jobs.size()>=32)return;std::vector<ID3D12CommandList*> lists(original,original+count);Job job;
        for(UINT i=0;i<count;++i){auto it=s.commands.find(reinterpret_cast<ID3D12GraphicsCommandList*>(original[i]));if(it==s.commands.end())continue;auto c=it->second;
            if(c->valid&&c->closed&&c->epoch==s.epoch&&c->modified){lists[i]=c->list.Get();job.commands.push_back(c);}}
        if(job.commands.empty())return;Internal guard;
        if(FAILED(queue->GetDevice(IID_PPV_ARGS(&job.device)))||FAILED(job.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&job.fence))))return;
        // Reserve lifetime storage before submitting any work. Original lists stay
        // untouched: disabling the experiment selects them even on cached replay.
        s.jobs.push_back(std::move(job));auto& retained=s.jobs.back();
        queue->ExecuteCommandLists(count,lists.data());replaced=true;
        retained.signaled=SUCCEEDED(queue->Signal(retained.fence.Get(),1));
        if(!retained.signaled){++s.faults;s.rate=0;}
        ++s.modified_submissions;for(const auto& c:retained.commands)s.modified_draws+=c->modified;
    });return replaced;
}
void collect()noexcept{safe([&]{auto& s=state();if(s.rate&&std::chrono::steady_clock::now()>=s.expires)configure(0);auto& jobs=s.jobs;std::erase_if(jobs,[](const Job& j){return (j.signaled&&j.fence->GetCompletedValue()>=1)||FAILED(j.device->GetDeviceRemovedReason());});});}
void snapshot(std::ostream& out){std::lock_guard lock(state().mutex);const auto& s=state();out<<"{\"experimental\":true,\"requested_rate\":"<<s.rate.load()<<",\"modified_submissions\":"<<s.modified_submissions<<",\"modified_draws\":"<<s.modified_draws<<",\"tracked_recordings\":"<<s.commands.size()<<",\"inflight_batches\":"<<s.jobs.size()<<",\"skipped\":"<<s.skipped<<",\"faults\":"<<s.faults<<",\"image_quality_verified\":false}";}
}
