#include "generic_cpu_workers.hpp"
#include "generic_command_mirror.hpp"
#include "arc/intercept_cpu_meter.hpp"
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
#include <stdexcept>
#include <iomanip>

namespace arc::dx12::mirror {
namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
thread_local bool nested{};
struct Internal {bool old{nested};Internal(){nested=true;}~Internal(){nested=old;}};
void checked(HRESULT hr){if(FAILED(hr))throw std::runtime_error("VRS control resource HRESULT "+std::to_string(hr));}
// The bounded pool retains maps until all GPU use is fenced. A slot can be
// reused only after its native command list has been destroyed and completed.
struct Control {
    Ptr<ID3D12Device> device;
    Ptr<ID3D12Resource> image,upload;
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> copies[9]; // initialize + four on/off pairs
    UINT64 reusable_after[4]{};
    int active_slot{-1},retired_slot{-1};
    Ptr<ID3D12Fence> fence;
    Ptr<ID3D12CommandQueue> queue;
    UINT64 value{};
    UINT rate{};
    bool initialized{},failed{};
};
struct Command {
    std::shared_ptr<Control> control;
    ID3D12GraphicsCommandList5* v5{}; // borrowed; retired by native lifetime token
    UINT rate{};
    D3D12_SHADING_RATE_COMBINER combiners[2]{};
    std::uint64_t epoch{},modified{},potential{};
    bool bound{},app_image{},eligible_pipeline{},closed{},valid{true},qualified{true};
};
struct State {
    std::recursive_mutex mutex;
    std::atomic<UINT> rate{};
    std::chrono::steady_clock::time_point expires;
    char last_error[160]{};
    std::uint64_t epoch{},modified_submissions{},modified_draws{},skipped{},faults{},map_updates{},policy_waits{};
    std::atomic<std::uint64_t> recorded_draws{};
    std::map<ID3D12GraphicsCommandList*,std::shared_ptr<Command>> commands;
    std::vector<std::shared_ptr<Control>> controls;
    std::map<ID3D12PipelineState*,bool> pipelines;
    std::map<ID3D12RootSignature*,bool> roots;
    std::map<ID3D12CommandSignature*,bool> signatures;
    std::set<ID3D12GraphicsCommandList*> command_lifetimes;
};
State& state(){static auto* s=new State;return *s;}
constexpr GUID lifetime_guid{0x614ed930,0x83d7,0x44ca,{0xb9,0x06,0x35,0x9e,0x65,0x1d,0x3b,0x1a}};
class Lifetime final:public IUnknown {
    std::atomic<ULONG> count{1};void* object;int kind;
public:
    Lifetime(void* p,int k):object(p),kind(k){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown)return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++count;}
    ULONG STDMETHODCALLTYPE Release()override{arc::InterceptCpuMeter::Scope cpu_hook(!cpu_cost::on_worker_thread());const auto n=--count;if(!n){auto& s=state();{std::lock_guard lock(s.mutex);if(kind==1){s.commands.erase(static_cast<ID3D12GraphicsCommandList*>(object));s.command_lifetimes.erase(static_cast<ID3D12GraphicsCommandList*>(object));}else if(kind==2)s.signatures.erase(static_cast<ID3D12CommandSignature*>(object));else if(kind==3)s.roots.erase(static_cast<ID3D12RootSignature*>(object));else s.pipelines.erase(static_cast<ID3D12PipelineState*>(object));}delete this;}return n;}
};
bool track(ID3D12Object* object,int kind){auto* token=new Lifetime(object,kind);const auto hr=object->SetPrivateDataInterface(lifetime_guid,token);token->Release();return SUCCEEDED(hr);}
template<class F>void safe(F&& fn)noexcept{try{std::lock_guard lock(state().mutex);fn();}catch(const std::exception& e){std::lock_guard lock(state().mutex);state().rate=0;++state().faults;strncpy_s(state().last_error,e.what(),_TRUNCATE);}catch(...){std::lock_guard lock(state().mutex);state().rate=0;++state().faults;}}
std::shared_ptr<Control> make_control(ID3D12Device* device,UINT tile){
    auto c=std::make_shared<Control>();c->device=device;
    // 4096x4096 render-pixel coverage; pixels outside the image remain 1x1.
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=(4096+tile-1)/tile;
    d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8_UINT;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    checked(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&c->image)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};device->GetCopyableFootprints(&d,0,1,0,&footprint,nullptr,nullptr,&bytes);
    const UINT64 stride=(bytes+511)&~UINT64(511);
    D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=stride*2;b.Height=b.DepthOrArraySize=b.MipLevels=b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hp.Type=D3D12_HEAP_TYPE_UPLOAD;checked(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&c->upload)));
    void* data{};D3D12_RANGE empty{};checked(c->upload->Map(0,&empty,&data));std::memset(data,0,static_cast<size_t>(stride));std::memset(static_cast<char*>(data)+stride,D3D12_SHADING_RATE_2X2,static_cast<size_t>(stride));c->upload->Unmap(0,nullptr);
    checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&c->allocator)));
    for(UINT i=0;i<9;++i){
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,c->allocator.Get(),nullptr,IID_PPV_ARGS(&c->copies[i])));
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={c->image.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST};
        if(i)c->copies[i]->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=c->image.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.pResource=c->upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;src.PlacedFootprint.Offset=i&&i%2?stride:0;
        c->copies[i]->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);c->copies[i]->ResourceBarrier(1,&barrier);checked(c->copies[i]->Close());
    }
    checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&c->fence)));return c;
}
void restore(Command& c){
    if(!c.bound)return;Internal guard;c.v5->RSSetShadingRateImage(nullptr);
    c.v5->RSSetShadingRate(static_cast<D3D12_SHADING_RATE>(c.rate),c.combiners);c.bound=false;
}

}
bool internal()noexcept{return nested;}
UINT requested_rate()noexcept{return state().rate.load();}
std::uint64_t modified_draws()noexcept{std::lock_guard lock(state().mutex);return state().modified_draws;}
InternalCall::InternalCall()noexcept:old(nested){nested=true;}
InternalCall::~InternalCall(){nested=old;}
bool configure(UINT rate)noexcept{
    if(rate!=0&&rate!=D3D12_SHADING_RATE_2X2)return false;
    safe([&]{auto& s=state();++s.epoch;s.rate=rate;s.expires=std::chrono::steady_clock::now()+std::chrono::seconds(60);});return true;
}
void keep_alive()noexcept{safe([&]{state().expires=std::chrono::steady_clock::now()+std::chrono::seconds(60);});}
bool restoration_ready()noexcept{bool ready=false;safe([&]{auto& s=state();ready=!s.rate&&std::all_of(s.controls.begin(),s.controls.end(),[](const auto& c){return !c->failed&&c->fence->GetCompletedValue()!=UINT64_MAX&&c->fence->GetCompletedValue()>=c->value;});});return ready;}
void root_created(ID3D12RootSignature* root,const void* data,SIZE_T bytes)noexcept{
    if(!root||!data||!bytes)return;
    safe([&]{auto& s=state();if(s.roots.contains(root)||s.roots.size()>=16384||!track(root,3))return;s.roots[root]=false;
        Ptr<ID3D12VersionedRootSignatureDeserializer> reader;
        if(FAILED(D3D12CreateVersionedRootSignatureDeserializer(data,bytes,IID_PPV_ARGS(&reader))))return;
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* version{};
        if(FAILED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1,&version))||!version)return;
        const auto& d=version->Desc_1_1;
        if(d.Flags&D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)return;
        for(UINT i=0;i<d.NumParameters;++i){const auto& p=d.pParameters[i];
            if(p.ParameterType==D3D12_ROOT_PARAMETER_TYPE_UAV)return;
            if(p.ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)for(UINT j=0;j<p.DescriptorTable.NumDescriptorRanges;++j)if(p.DescriptorTable.pDescriptorRanges[j].RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_UAV)return;
        }
        s.roots[root]=true;
    });
}
void begin(ID3D12GraphicsCommandList* native,ID3D12PipelineState* pso)noexcept{
    if(nested)return;
    safe([&]{auto& s=state();auto previous=s.commands.find(native);std::shared_ptr<Command> c;
        if(previous!=s.commands.end()){
            c=previous->second;const bool qualified=c->valid;auto control=c->control;auto* v5=c->v5;
            if(control->fence->GetCompletedValue()<control->value){
                // Reset may overlap a preceding execution when the application
                // supplies another allocator. Its GPU generation keeps its map;
                // never serialize independent queues through a recycled image.
                s.commands.erase(previous);c.reset();
            }else{*c=Command{};c->control=std::move(control);c->v5=v5;c->qualified=qualified;}
        }
        if(!s.rate){return;}
        if(!c){
            if(native->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
            if(s.commands.size()>=16384){++s.skipped;return;}Internal guard;
            Ptr<ID3D12Device> device;checked(native->GetDevice(IID_PPV_ARGS(&device)));D3D12_FEATURE_DATA_D3D12_OPTIONS6 caps{};
            if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,&caps,sizeof(caps)))||caps.VariableShadingRateTier<D3D12_VARIABLE_SHADING_RATE_TIER_2||!caps.ShadingRateImageTileSize){++s.skipped;return;}
            std::shared_ptr<Control> control;
            for(const auto& available:s.controls)if(available.use_count()==1&&available->device.Get()==device.Get()&&!available->failed&&available->fence->GetCompletedValue()>=available->value){control=available;break;}
            if(!control){if(s.controls.size()>=128){++s.skipped;return;}control=make_control(device.Get(),caps.ShadingRateImageTileSize);s.controls.push_back(control);}
            c=std::make_shared<Command>();c->control=std::move(control);
            Ptr<ID3D12GraphicsCommandList5> v5;checked(native->QueryInterface(IID_PPV_ARGS(&v5)));c->v5=v5.Get();
            if(!s.command_lifetimes.contains(native)){if(s.command_lifetimes.size()>=16384||!track(native,1))return;s.command_lifetimes.insert(native);}s.commands[native]=c;
        }
        c->epoch=s.epoch;c->eligible_pipeline=pso&&s.pipelines.contains(pso)&&s.pipelines.at(pso);
    });
}
Lease acquire(ID3D12GraphicsCommandList*)noexcept{return {};}
Lease acquire_draw(ID3D12GraphicsCommandList* native)noexcept{
    Lease lease;if(nested||!state().rate.load(std::memory_order_relaxed))return lease;
    safe([&]{auto& s=state();auto it=s.commands.find(native);if(it==s.commands.end())return;auto& c=it->second;
        if(c->eligible_pipeline)++c->potential;
        if(c->valid&&c->qualified&&!c->closed&&c->epoch==s.epoch&&c->eligible_pipeline&&!c->app_image&&c->rate==D3D12_SHADING_RATE_1X1&&c->combiners[0]==D3D12_SHADING_RATE_COMBINER_PASSTHROUGH&&c->combiners[1]==D3D12_SHADING_RATE_COMBINER_PASSTHROUGH){lease.owner=c;lease.list=native;}
    });return lease;
}
void close(ID3D12GraphicsCommandList* native)noexcept{if(nested)return;safe([&]{auto it=state().commands.find(native);if(it!=state().commands.end()&&!it->second->closed){restore(*it->second);it->second->closed=true;}});}
void invalidate(ID3D12GraphicsCommandList* native)noexcept{if(nested)return;safe([&]{auto it=state().commands.find(native);if(it!=state().commands.end()){restore(*it->second);it->second->valid=false;}});}
void pipeline_created(ID3D12PipelineState* pso,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc)noexcept{
    if(nested||!pso||!desc)return;safe([&]{auto& s=state();if(s.pipelines.contains(pso)||s.pipelines.size()>=16384)return;if(!track(pso,false))return;
        s.pipelines[pso]=s.roots.contains(desc->pRootSignature)&&s.roots.at(desc->pRootSignature)&&desc->PS.pShaderBytecode&&desc->PS.BytecodeLength&&desc->SampleDesc.Count>=1&&desc->SampleDesc.Count<=4&&desc->RasterizerState.ForcedSampleCount==0;});
}
void signature_created(ID3D12CommandSignature* signature,const D3D12_COMMAND_SIGNATURE_DESC* desc)noexcept{
    if(nested||!signature||!desc)return;safe([&]{auto& s=state();if(s.signatures.contains(signature)||s.signatures.size()>=16384||!track(signature,2))return;
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
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:ok=read(&desc.pRootSignature);break;
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
    if(nested||!state().rate)return;safe([&]{auto& s=state();auto it=s.commands.find(native);if(it!=s.commands.end()){auto& c=*it->second;c.eligible_pipeline=pso&&s.pipelines.contains(pso)&&s.pipelines.at(pso);if(!c.eligible_pipeline)restore(c);}});
}
void shading_rate(ID3D12GraphicsCommandList* native,D3D12_SHADING_RATE rate,const D3D12_SHADING_RATE_COMBINER* combiners)noexcept{
    if(nested||!state().rate)return;safe([&]{auto it=state().commands.find(native);if(it==state().commands.end())return;auto& c=*it->second;restore(c);c.rate=rate;for(int i=0;i<2;++i)c.combiners[i]=combiners?combiners[i]:D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;});
}
void shading_image(ID3D12GraphicsCommandList* native,ID3D12Resource* image)noexcept{if(nested||!state().rate)return;safe([&]{auto it=state().commands.find(native);if(it!=state().commands.end()){restore(*it->second);it->second->app_image=image!=nullptr;}});}
bool before_draw(const Lease& lease)noexcept{
    if(!lease.owner)return false;auto c=std::static_pointer_cast<Command>(lease.owner);
    if(!c->bound){Internal guard;
        D3D12_SHADING_RATE_COMBINER combine[]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,D3D12_SHADING_RATE_COMBINER_OVERRIDE};
        c->v5->RSSetShadingRateImage(c->control->image.Get());c->v5->RSSetShadingRate(D3D12_SHADING_RATE_1X1,combine);c->bound=true;
    }
    ++c->modified;state().recorded_draws.fetch_add(1,std::memory_order_relaxed);return true;
}
void after_draw(const Lease&)noexcept{}

bool execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* original)noexcept{
    if(nested||!count||!original)return false;bool submitted=false;
    safe([&]{auto& s=state();if(s.rate&&std::chrono::steady_clock::now()>=s.expires)configure(0);
        struct Selection {Control* control;UINT desired;std::uint64_t draws;};
        Selection selected[128]{};UINT selected_count=0;
        for(UINT i=0;i<count;++i){
            auto it=s.commands.find(reinterpret_cast<ID3D12GraphicsCommandList*>(original[i]));if(it==s.commands.end())continue;
            auto& command=*it->second;if(!command.modified)continue;
            auto* control=command.control.get();UINT index=0;while(index<selected_count&&selected[index].control!=control)++index;
            if(index==selected_count){selected[index]={control,s.rate.load(),0};++selected_count;}
            selected[index].draws+=command.modified;
            if(!command.valid||!command.closed||command.epoch!=s.epoch)selected[index].desired=0;
        }
        if(!selected_count)return;Internal guard;
        for(UINT i=0;i<selected_count;++i){auto& choice=selected[i];auto& c=*choice.control;
            if(c.failed){s.rate=0;++s.faults;return;}
            // Every native list has its own map: independent queues/lists never
            // acquire artificial dependencies on one another. Cached migration
            // only waits for that same list's preceding submission on the GPU.
            if(c.queue&&c.queue.Get()!=queue&&c.fence->GetCompletedValue()<c.value){
                if(FAILED(queue->Wait(c.fence.Get(),c.value))){c.failed=true;++s.faults;s.rate=0;return;}++s.policy_waits;
            }
            if(!c.initialized){ID3D12CommandList* init[]{c.copies[0].Get()};queue->ExecuteCommandLists(1,init);c.initialized=true;c.rate=0;++s.map_updates;}
            if(choice.desired&&c.rate==0){
                int slot=-1;for(int k=0;k<4;++k)if(c.fence->GetCompletedValue()>=c.reusable_after[k]){slot=k;break;}
                // Reserve the unused neutral helper BEFORE enabling. If all
                // pairs are in flight, stay at 1x1 without a CPU wait or loss of
                // rollback capacity. Never replay an in-flight helper list.
                if(slot<0){choice.desired=0;++s.skipped;}
                else{ID3D12CommandList* update[]{c.copies[1+slot*2].Get()};queue->ExecuteCommandLists(1,update);c.active_slot=slot;c.rate=D3D12_SHADING_RATE_2X2;++s.map_updates;}
            }else if(!choice.desired&&c.rate){
                ID3D12CommandList* update[]{c.copies[2+c.active_slot*2].Get()};queue->ExecuteCommandLists(1,update);
                c.retired_slot=c.active_slot;c.active_slot=-1;c.rate=0;++s.map_updates;
            }
        }
        {arc::InterceptCpuMeter::Native application_work;queue->ExecuteCommandLists(count,original);}submitted=true;bool modified=false;
        for(UINT i=0;i<selected_count;++i){auto& c=*selected[i].control;c.queue=queue;
            if(FAILED(queue->Signal(c.fence.Get(),++c.value))){c.failed=true;++s.faults;s.rate=0;}
            if(c.retired_slot>=0){c.reusable_after[c.retired_slot]=c.value;c.retired_slot=-1;}
            if(c.rate){modified=true;s.modified_draws+=selected[i].draws;}
        }
        if(modified)++s.modified_submissions;else ++s.skipped;
    });return submitted;
}
void collect()noexcept{safe([&]{auto& s=state();if(s.rate&&std::chrono::steady_clock::now()>=s.expires)configure(0);});}
void snapshot(std::ostream& out){std::lock_guard lock(state().mutex);const auto& s=state();out<<"{\"experimental\":true,\"backend\":\"tier2_reversible_rate_image\",\"requested_rate\":"<<s.rate.load()<<",\"modified_submissions\":"<<s.modified_submissions<<",\"modified_draws\":"<<s.modified_draws<<",\"tracked_recordings\":"<<s.commands.size()<<",\"inflight_batches\":0,\"native_command_copies\":0,\"recorded_controlled_draws\":"<<s.recorded_draws<<",\"map_updates\":"<<s.map_updates<<",\"cross_queue_gpu_waits\":"<<s.policy_waits<<",\"control_images\":"<<s.controls.size()<<",\"skipped\":"<<s.skipped<<",\"faults\":"<<s.faults<<",\"last_error\":"<<std::quoted(s.last_error)<<",\"image_quality_verified\":false}";}
}
