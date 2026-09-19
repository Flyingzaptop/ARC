#include "generic_runtime.hpp"
#include "generic_readback.hpp"
#include "arc/gpu_attribution.hpp"
#include "arc/descriptor_ledger.hpp"
#include <atomic>
#include <map>
#include <mutex>
#include <fstream>
#include <set>
#include <algorithm>
#include <source_location>
#include <iomanip>
#include <memory>

namespace arc::dx12::generic {
namespace {
constexpr GUID token_guid{0xa337b583,0x9b46,0x46bc,{0x94,0x10,0x61,0x5f,0x3a,0x41,0x29,0x6d}};
enum class Kind {Resource, Heap, Command, Pipeline, Queue, RootSignature, Fence, Swapchain};
struct Object {std::uint64_t id{};Kind kind{};D3D12_RESOURCE_DESC resource{};D3D12_DESCRIPTOR_HEAP_DESC heap{};UINT stride{};UINT64 cpu{},gpu{},gpu_address{};};
struct View {std::uint64_t resource{},heap{};UINT kind{},first_mip{},mips{};};
struct Recording {std::uint64_t id{},pipeline{},graphics_root{},compute_root{};std::vector<std::uint64_t> targets,heaps;std::map<UINT,UINT64> graphics_tables,compute_tables;RasterRegion raster{};bool viewport{},scissor{},started{};};
struct State {
    std::recursive_mutex mutex;
    std::map<std::uintptr_t,Object> objects;
    std::map<std::uint64_t,Object*> by_id;
    std::map<UINT64,std::uint64_t> cpu_heaps,gpu_heaps;
    std::multimap<UINT64,std::uint64_t> gpu_buffers;
    UINT64 maximum_buffer_width{};
    DescriptorLedger ledger;
    std::map<std::uint64_t,Recording> commands;
    std::map<std::uint64_t,std::uint64_t> swapchain_queues;
    std::map<std::uint64_t,Microsoft::WRL::ComPtr<ID3D12CommandQueue>> image_queues;
    std::filesystem::path image_path;
    std::unique_ptr<ImageReadback> image;
    std::uint64_t image_swapchain{},images_completed{},images_failed{};
    std::atomic<bool> image_pending{};
    bool image_writing{};
    std::atomic<std::uint64_t> unsupported_calls{};
    std::filesystem::path requested_path;
    std::uint64_t capture_swapchain{},present_resource{},present_queue{};
    bool armed{},ready{};
    GpuAttributionGraph graph;
    std::uint64_t next{1},created{},retired{},errors{},descriptor_writes{},submitted{},captured{},unknown_tables{},untracked_heap_writes{};
    std::atomic<bool> capturing{};
    struct ErrorSite {const char* function{};std::uint64_t count{};};std::map<unsigned,ErrorSite> error_sites;
    bool complete{};
};
State& state(){static auto* s=new State;return *s;} // lifetime spans application shutdown callbacks
void invalidate(std::source_location location=std::source_location::current()){auto& s=state();++s.errors;s.complete=false;if(s.capturing)s.graph.invalidate();if(s.error_sites.size()<32||s.error_sites.contains(location.line())){auto& site=s.error_sites[location.line()];site.function=location.function_name();++site.count;}}
void retire(std::uintptr_t address,std::uint64_t id) noexcept {
    auto& s=state();std::lock_guard lock(s.mutex);auto it=s.objects.find(address);
    if(it==s.objects.end()||it->second.id!=id)return;
    if(it->second.kind==Kind::Heap){
        const auto cpu=s.cpu_heaps.find(it->second.cpu);if(cpu!=s.cpu_heaps.end()&&cpu->second==id)s.cpu_heaps.erase(cpu);
        const auto gpu=s.gpu_heaps.find(it->second.gpu);if(gpu!=s.gpu_heaps.end()&&gpu->second==id)s.gpu_heaps.erase(gpu);
    }
    if(it->second.gpu_address){auto [first,last]=s.gpu_buffers.equal_range(it->second.gpu_address);for(auto cursor=first;cursor!=last;){if(cursor->second==id)cursor=s.gpu_buffers.erase(cursor);else ++cursor;}}
    if(it->second.kind==Kind::Heap)s.ledger.retire_heap(id);
    s.swapchain_queues.erase(id);s.image_queues.erase(id);if(it->second.kind==Kind::Queue)std::erase_if(s.swapchain_queues,[&](const auto& pair){return pair.second==id;});
    s.commands.erase(id);s.graph.retire_command(id);s.by_id.erase(id);s.objects.erase(it);++s.retired;
}
class Lifetime final:public IUnknown {
    std::atomic<ULONG> references{1};std::uintptr_t address;std::uint64_t id;
public:
    Lifetime(std::uintptr_t a,std::uint64_t i):address(a),id(i){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override {if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown)return E_NOINTERFACE;*out=static_cast<IUnknown*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override {return ++references;}
    ULONG STDMETHODCALLTYPE Release()override {const auto n=--references;if(!n){retire(address,id);delete this;}return n;}
};
std::uint64_t identify(ID3D12Object* object,Kind kind){
    if(!object)return 0;
    auto& s=state();const auto key=reinterpret_cast<std::uintptr_t>(object);
    if(const auto found=s.objects.find(key);found!=s.objects.end())return found->second.id;
    if(s.objects.size()>=16384){invalidate();return 0;}
    const auto id=s.next++;auto inserted=s.objects.emplace(key,Object{id,kind});s.by_id[id]=&inserted.first->second;
    auto* token=new Lifetime(key,id);const auto hr=object->SetPrivateDataInterface(token_guid,token);token->Release();
    if(FAILED(hr)){s.by_id.erase(id);s.objects.erase(key);invalidate();return 0;}
    ++s.created;return id;
}
Object* find_id(std::uint64_t id){auto& s=state();const auto it=s.by_id.find(id);return it==s.by_id.end()?nullptr:it->second;}
std::uint64_t swapchain_id(IDXGISwapChain* swap){
    auto& s=state();const auto key=reinterpret_cast<std::uintptr_t>(swap);if(const auto it=s.objects.find(key);it!=s.objects.end())return it->second.id;
    if(s.objects.size()>=16384){invalidate();return 0;}const auto id=s.next++;auto inserted=s.objects.emplace(key,Object{id,Kind::Swapchain});s.by_id[id]=&inserted.first->second;
    auto* token=new Lifetime(key,id);const auto hr=swap->SetPrivateDataInterface(token_guid,token);token->Release();if(FAILED(hr)){s.by_id.erase(id);s.objects.erase(key);invalidate();return 0;}++s.created;return id;
}
std::uint64_t resource_id(ID3D12Resource* resource){
    if(!resource)return 0;const auto id=identify(resource,Kind::Resource);
    if(id){auto& s=state();auto& object=s.objects.at(reinterpret_cast<std::uintptr_t>(resource));if(object.resource.Width)return id;object.resource=resource->GetDesc();if(object.resource.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER){object.gpu_address=resource->GetGPUVirtualAddress();if(object.gpu_address)s.gpu_buffers.emplace(object.gpu_address,id);s.maximum_buffer_width=std::max(s.maximum_buffer_width,object.resource.Width);}}return id;
}
Object* heap_at(UINT64 handle,bool gpu){
    auto& index=gpu?state().gpu_heaps:state().cpu_heaps;auto it=index.upper_bound(handle);if(it==index.begin())return nullptr;--it;
    auto* o=find_id(it->second);if(!o||!o->stride)return nullptr;const auto delta=handle-it->first;
    return delta/o->stride<o->heap.NumDescriptors&&delta%o->stride==0?o:nullptr;
}
Recording* command(ID3D12GraphicsCommandList* native){
    const auto id=identify(native,Kind::Command);if(!id)return nullptr;auto& s=state();
    if(!s.commands.contains(id)){if(s.commands.size()>=256){invalidate();return nullptr;}s.commands.emplace(id,Recording{id});}
    return &s.commands.at(id);
}
std::optional<View> descriptor(UINT64 address){
    auto& s=state();const auto value=s.ledger.read(address);if(!value)return {};
    if(value->resource&&!find_id(value->resource))return {}; // retired identity never becomes a new resource
    return View{value->resource,s.ledger.heap_at(address),value->kind,value->first_mip,value->mips};
}
void assign(UINT64 address,View value){
    auto& s=state();if(!s.ledger.write(address,{value.resource,value.kind,value.first_mip,value.mips})){s.ledger.forget(address);invalidate();return;}
    ++s.descriptor_writes;if(!s.ledger.heap_at(address))++s.untracked_heap_writes;
}
void forget(UINT64 address){state().ledger.forget(address);}
template<class F>void safe(F&& action)noexcept{try{auto& s=state();std::lock_guard lock(s.mutex);action();}catch(...){auto& s=state();std::lock_guard lock(s.mutex);invalidate();}}
void add_access(WorkObservation& w,std::uint64_t id,bool write,AccessEvidence evidence,bool full=false){
    if(!id)return;
    if(w.accesses.size()>=64){invalidate();return;}
    for(const auto& a:w.accesses)if(a.resource==id&&a.write==write)return;
    w.accesses.push_back({id,write,evidence,full});
}
void record(Recording& c,WorkObservation w){
    auto& s=state();if(!s.capturing)return;
    if(!c.started){invalidate();return;}
    if(!s.graph.record(c.id,w))s.complete=false;else ++s.captured;
}
}
void observe_resource(ID3D12Resource* r)noexcept{safe([&]{resource_id(r);});}
void observe_heap(ID3D12DescriptorHeap* h)noexcept{safe([&]{
    const auto id=identify(h,Kind::Heap);if(!id)return;auto& s=state();auto& o=s.objects.at(reinterpret_cast<std::uintptr_t>(h));if(o.stride)return;o.heap=h->GetDesc();o.cpu=h->GetCPUDescriptorHandleForHeapStart().ptr;
    if(o.heap.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)o.gpu=h->GetGPUDescriptorHandleForHeapStart().ptr;
    ID3D12Device* device=nullptr;if(SUCCEEDED(h->GetDevice(IID_PPV_ARGS(&device)))){o.stride=device->GetDescriptorHandleIncrementSize(o.heap.Type);device->Release();}else invalidate();
    if(o.cpu)s.cpu_heaps[o.cpu]=id;if(o.gpu)s.gpu_heaps[o.gpu]=id;
    if(!s.ledger.register_heap(id,o.cpu,o.stride,o.heap.NumDescriptors))invalidate();
});}
void observe_view(ID3D12Resource* r,D3D12_CPU_DESCRIPTOR_HANDLE handle,UINT kind,UINT first,UINT mips)noexcept{safe([&]{
    const auto id=resource_id(r);assign(handle.ptr,{id,0,(!r&&kind!=5)?0:kind,first,mips});
});}
void observe_cbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{safe([&]{
    auto& s=state();std::uint64_t id=0;
    if(desc){auto begin=s.gpu_buffers.lower_bound(desc->BufferLocation>s.maximum_buffer_width?desc->BufferLocation-s.maximum_buffer_width:0),end=s.gpu_buffers.upper_bound(desc->BufferLocation);
        for(auto it=begin;it!=end;++it){const auto* o=find_id(it->second);if(!o)continue;const auto offset=desc->BufferLocation-it->first;
            if(offset<=o->resource.Width&&desc->SizeInBytes<=o->resource.Width-offset){if(id){id=0;break;}id=o->id;}}}
    assign(handle.ptr,{id,0,desc?6u:0u,0,0});
});}
void copy_descriptors(UINT count,D3D12_CPU_DESCRIPTOR_HANDLE dst,D3D12_CPU_DESCRIPTOR_HANDLE src,D3D12_DESCRIPTOR_HEAP_TYPE type,UINT stride)noexcept{safe([&]{
    if(count>4096){invalidate();return;}auto* source=heap_at(src.ptr,false);auto* destination=heap_at(dst.ptr,false);
    if(!stride||(source&&source->heap.Type!=type)||(destination&&destination->heap.Type!=type)){invalidate();return;}
    std::vector<View> copies;copies.reserve(count);
    for(UINT i=0;i<count;++i){auto* sh=heap_at(src.ptr+UINT64(i)*stride,false);auto* dh=heap_at(dst.ptr+UINT64(i)*stride,false);if((source&&sh!=source)||(destination&&dh!=destination)){invalidate();return;}
        const auto value=descriptor(src.ptr+UINT64(i)*stride);if(!value){for(UINT k=0;k<count;++k)forget(dst.ptr+UINT64(k)*stride);invalidate();return;}copies.push_back(*value);}
    for(UINT i=0;i<count;++i)assign(dst.ptr+UINT64(i)*stride,copies[i]);
});}
void descriptor_ranges(UINT dst_count,const D3D12_CPU_DESCRIPTOR_HANDLE* dst,const UINT* dst_sizes,UINT src_count,const D3D12_CPU_DESCRIPTOR_HANDLE* src,const UINT* src_sizes,D3D12_DESCRIPTOR_HEAP_TYPE type,UINT stride)noexcept{safe([&]{
    if(!stride||dst_count>1024||src_count>1024){invalidate();return;}std::vector<UINT64> destinations;std::vector<View> sources;
    for(UINT range=0;range<dst_count;++range){auto* h=heap_at(dst[range].ptr,false);const UINT count=dst_sizes?dst_sizes[range]:1;
        if((h&&h->heap.Type!=type)||count>4096||destinations.size()+count>4096){invalidate();return;}
        for(UINT i=0;i<count;++i){const auto address=dst[range].ptr+UINT64(i)*stride;if(h&&heap_at(address,false)!=h){invalidate();return;}destinations.push_back(address);}
    }
    for(UINT range=0;range<src_count;++range){auto* h=heap_at(src[range].ptr,false);const UINT count=src_sizes?src_sizes[range]:1;
        if((h&&h->heap.Type!=type)||count>4096||sources.size()+count>4096){invalidate();return;}
        for(UINT i=0;i<count;++i){const auto address=src[range].ptr+UINT64(i)*stride;const auto value=descriptor(address);if((h&&heap_at(address,false)!=h)||!value){for(auto target:destinations)forget(target);invalidate();return;}sources.push_back(*value);}
    }
    if(sources.size()!=destinations.size()){invalidate();return;}
    for(std::size_t i=0;i<sources.size();++i)assign(destinations[i],sources[i]);
});}
void begin(ID3D12GraphicsCommandList* n)noexcept{if(!state().capturing.load(std::memory_order_relaxed))return;safe([&]{if(auto* c=command(n)){const auto id=c->id;*c=Recording{id};c->started=state().capturing;if(c->started&&!state().graph.begin(id))invalidate();}});}
void close(ID3D12GraphicsCommandList* n)noexcept{if(!state().capturing.load(std::memory_order_relaxed))return;safe([&]{if(state().capturing)if(auto* c=command(n)){if(!c->started||!state().graph.close(c->id))invalidate();}});}
void pipeline(ID3D12GraphicsCommandList* n,ID3D12PipelineState* p)noexcept{if(!state().capturing.load(std::memory_order_relaxed))return;safe([&]{if(auto* c=command(n))c->pipeline=identify(p,Kind::Pipeline);});}
void targets(ID3D12GraphicsCommandList* n,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* handles,BOOL contiguous,const D3D12_CPU_DESCRIPTOR_HANDLE* depth)noexcept{if(!state().capturing.load(std::memory_order_relaxed))return;safe([&]{
    auto* c=command(n);if(!c)return;c->targets.clear();c->raster.width=c->raster.height=0;
    if(count>8||(!handles&&count)){invalidate();return;}
    UINT stride=0;if(count&&contiguous){auto* h=heap_at(handles[0].ptr,false);if(!h){invalidate();return;}stride=h->stride;}
    for(UINT i=0;i<count+(depth?1:0);++i){const auto address=i==count?depth->ptr:contiguous?handles[0].ptr+UINT64(i)*stride:handles[i].ptr;
        const auto v=descriptor(address);if(!v){invalidate();continue;}c->targets.push_back(v->resource);
        if(i==0){auto* r=find_id(v->resource);if(r&&r->resource.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&v->first_mip<r->resource.MipLevels){const auto shift=std::min(v->first_mip,31u);c->raster.width=static_cast<UINT>(std::max<UINT64>(1,std::min<UINT64>(r->resource.Width,UINT_MAX)>>shift));c->raster.height=std::max(1u,r->resource.Height>>shift);}}
    }
    c->raster.known=count==1&&c->viewport&&c->scissor&&c->raster.width&&c->raster.height;
});}
void viewport(ID3D12GraphicsCommandList* n,UINT count,const D3D12_VIEWPORT* v)noexcept{if(!state().capturing.load(std::memory_order_relaxed))return;safe([&]{if(auto* c=command(n)){c->viewport=count==1&&v;if(c->viewport)c->raster.viewport={v->TopLeftX,v->TopLeftY,v->Width,v->Height};c->raster.known=c->viewport&&c->scissor&&c->targets.size()==1&&c->raster.width&&c->raster.height;}});}
void scissor(ID3D12GraphicsCommandList* n,UINT count,const D3D12_RECT* v)noexcept{if(!state().capturing.load(std::memory_order_relaxed))return;safe([&]{if(auto* c=command(n)){c->scissor=count==1&&v;if(c->scissor)c->raster.scissor={double(v->left),double(v->top),double(v->right)-v->left,double(v->bottom)-v->top};c->raster.known=c->viewport&&c->scissor&&c->targets.size()==1&&c->raster.width&&c->raster.height;}});}
void descriptor_heaps(ID3D12GraphicsCommandList* n,UINT count,ID3D12DescriptorHeap*const* heaps)noexcept{safe([&]{auto* c=command(n);if(!c)return;if(count>2){invalidate();return;}std::vector<std::uint64_t> next;for(UINT i=0;i<count;++i){observe_heap(heaps[i]);next.push_back(identify(heaps[i],Kind::Heap));}if(next!=c->heaps){c->graphics_tables.clear();c->compute_tables.clear();c->heaps=std::move(next);}});}
void root_table(ID3D12GraphicsCommandList* n,bool compute,UINT parameter,D3D12_GPU_DESCRIPTOR_HANDLE handle)noexcept{if(!state().capturing.load(std::memory_order_relaxed))return;safe([&]{if(parameter>=64){invalidate();return;}if(auto* c=command(n))(compute?c->compute_tables:c->graphics_tables)[parameter]=handle.ptr;});}
void root_signature(ID3D12GraphicsCommandList* n,bool compute,ID3D12RootSignature* root)noexcept{if(!state().capturing.load(std::memory_order_relaxed))return;safe([&]{if(auto* c=command(n)){const auto id=identify(root,Kind::RootSignature);auto& current=compute?c->compute_root:c->graphics_root;if(current!=id){(compute?c->compute_tables:c->graphics_tables).clear();current=id;}}});}
void work(ID3D12GraphicsCommandList* n,UINT kind,std::uint64_t items)noexcept{
    if(!state().capturing)return;
    safe([&]{auto* c=command(n);if(!c)return;WorkObservation w;w.kind=kind==2?GpuWorkKind::Dispatch:GpuWorkKind::Draw;w.items=items;w.pipeline=c->pipeline;w.raster=c->raster;
        // Root table range/dynamic indexing is unknown. Only the first descriptor
        // is a possible candidate; never claim complete shader access.
        for(const auto& [parameter,handle]:(kind==2?c->compute_tables:c->graphics_tables)){(void)parameter;auto* h=heap_at(handle,true);
            if(!h||std::find(c->heaps.begin(),c->heaps.end(),h->id)==c->heaps.end()){++state().unknown_tables;continue;}
            const auto value=descriptor(h->cpu+handle-h->gpu);if(!value){++state().unknown_tables;continue;}
            add_access(w,value->resource,value->kind==2,AccessEvidence::Possible);
        }
        if(kind!=2)for(auto id:c->targets)add_access(w,id,true,AccessEvidence::Possible);
        record(*c,std::move(w));
    });
}
void copy(ID3D12GraphicsCommandList* n,ID3D12Resource* src,ID3D12Resource* dst,UINT64 bytes,bool full)noexcept{
    if(!state().capturing)return;safe([&]{if(auto* c=command(n)){WorkObservation w;w.kind=GpuWorkKind::Copy;w.copy_bytes=bytes;w.bindings_complete=true;add_access(w,resource_id(src),false,AccessEvidence::Observed);add_access(w,resource_id(dst),true,AccessEvidence::Observed,full);record(*c,std::move(w));}});
}
void clear_target(ID3D12GraphicsCommandList* n,D3D12_CPU_DESCRIPTOR_HANDLE handle,bool full)noexcept{if(!state().capturing)return;safe([&]{auto it=descriptor(handle.ptr);if(!it){invalidate();return;}if(auto* c=command(n)){WorkObservation w;w.kind=GpuWorkKind::Clear;w.bindings_complete=true;const auto* r=find_id(it->resource);const bool whole=full&&r&&r->resource.MipLevels==1&&r->resource.DepthOrArraySize==1&&it->first_mip==0;add_access(w,it->resource,true,AccessEvidence::Observed,whole);record(*c,std::move(w));}});}
void submit(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists)noexcept{if(!state().capturing)return;safe([&]{auto& s=state();if(count>1024){invalidate();return;}const auto q=identify(queue,Kind::Queue);for(UINT i=0;i<count;++i){const auto id=identify(lists[i],Kind::Command);if(!s.commands.contains(id)||!s.commands.at(id).started){invalidate();continue;}s.graph.submit(q,id);++s.submitted;}});}
void fence(ID3D12CommandQueue* q,ID3D12Fence* f,UINT64 value,bool wait)noexcept{if(!state().capturing)return;safe([&]{const auto queue=identify(q,Kind::Queue),id=identify(f,Kind::Fence);if(wait){if(!state().graph.wait(queue,id,value))invalidate();}else if(!state().graph.signal(queue,id,value))invalidate();});}
void begin_capture()noexcept{safe([&]{auto& s=state();if(s.capturing)return;s.graph.clear();for(auto& [id,c]:s.commands){(void)id;c.started=false;}s.present_resource=s.present_queue=0;s.complete=true;s.capturing=true;s.captured=s.submitted=s.unknown_tables=0;});}
void end_capture(const std::filesystem::path& path)noexcept{safe([&]{auto& s=state();s.capturing=false;std::ofstream file(path);file<<"{\"schema\":1,\"engine_labels\":false,\"shader_access_complete\":false,\"present_resource\":"<<s.present_resource<<",\"present_queue\":"<<s.present_queue<<",\"present_queue_known\":"<<(s.present_queue?"true":"false")<<",\"capture_state_complete\":"<<(s.complete?"true":"false")<<",\"graph\":";s.graph.write_json(file);file<<'}';});}
void observe_swapchain(IDXGISwapChain* swap,IUnknown* native)noexcept{safe([&]{if(!swap||!native)return;Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;if(FAILED(native->QueryInterface(IID_PPV_ARGS(&queue))))return;const auto id=swapchain_id(swap),qid=identify(queue.Get(),Kind::Queue);if(id&&qid){state().swapchain_queues[id]=qid;state().image_queues[id]=queue;}});}
std::uint64_t before_present(IDXGISwapChain* swap)noexcept{
    if(!state().capturing&&!state().image_pending)return 0;std::uint64_t resource=0;
    safe([&]{auto& s=state();if(!s.image_pending||s.image)return;const auto id=swapchain_id(swap);const auto queue=s.image_queues.find(id);
        if(queue==s.image_queues.end()){++s.images_failed;s.image_pending=false;return;}
        s.image=std::make_unique<ImageReadback>();s.image_swapchain=id;
        if(!s.image->enqueue(swap,queue->second.Get(),s.image_path)){++s.images_failed;s.image_pending=false;if(!s.image->submitted())s.image.reset();}
    });
    if(!state().capturing)return 0;
    safe([&]{auto& s=state();if(s.capture_swapchain&&s.capture_swapchain!=swapchain_id(swap)){invalidate();return;}IDXGISwapChain3* current=nullptr;
        if(FAILED(swap->QueryInterface(IID_PPV_ARGS(&current)))){invalidate();return;}ID3D12Resource* buffer=nullptr;
        const auto hr=current->GetBuffer(current->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer));current->Release();
        if(SUCCEEDED(hr)){resource=resource_id(buffer);buffer->Release();}else invalidate();
    });return resource;
}
void after_present(IDXGISwapChain* swap,std::uint64_t resource,HRESULT result,UINT flags)noexcept{
    if(flags&DXGI_PRESENT_TEST)return;
    safe([&]{auto& s=state();if(s.image&&s.image_swapchain==swapchain_id(swap))s.image->presented(result);});
    safe([&]{auto& s=state();if(s.armed&&!s.capturing){if(FAILED(result))return;begin_capture();s.capture_swapchain=swapchain_id(swap);s.armed=false;return;}
        if(!s.capturing||s.requested_path.empty())return;
        if(swapchain_id(swap)!=s.capture_swapchain){invalidate();return;}
        if(FAILED(result)||!resource)invalidate();s.present_resource=resource;
        const auto queue=s.swapchain_queues.find(s.capture_swapchain);
        if(queue==s.swapchain_queues.end()){s.complete=false;}else{s.present_queue=queue->second;if(SUCCEEDED(result)&&resource)s.graph.present(queue->second,resource);}
        s.capturing=false;s.ready=true;
    });
}
bool request_frame(const std::filesystem::path& path)noexcept{bool accepted=false;safe([&]{auto& s=state();if(s.capturing||s.armed||s.ready||!path.is_absolute()||std::filesystem::exists(path))return;s.requested_path=path;s.armed=true;s.capture_swapchain=0;accepted=true;});return accepted;}
bool request_image(const std::filesystem::path& path)noexcept{bool accepted=false;safe([&]{auto& s=state();if(s.image_pending||s.image||s.image_writing||!path.is_absolute()||!std::filesystem::is_directory(path.parent_path())||std::filesystem::exists(path)||std::filesystem::exists(path.wstring()+L".pixels"))return;s.image_path=path;s.image_pending=true;accepted=true;});return accepted;}
void flush_image()noexcept{
    std::unique_ptr<ImageReadback> image;
    safe([&]{auto& s=state();if(s.image&&s.image->ready()){image=std::move(s.image);s.image_pending=false;s.image_writing=true;}});
    if(!image)return;bool ok=false;try{ok=image->write();}catch(...){}
    safe([&]{state().image_writing=false;if(ok)++state().images_completed;else ++state().images_failed;});
}
void flush_capture()noexcept{safe([&]{auto& s=state();if(!s.ready)return;end_capture(s.requested_path);s.ready=false;s.requested_path.clear();});}
void unsupported()noexcept{state().unsupported_calls.fetch_add(1,std::memory_order_relaxed);if(state().capturing.load(std::memory_order_relaxed))safe([]{invalidate();});}
void snapshot(std::ostream& out){auto& s=state();std::lock_guard lock(s.mutex);std::size_t resources=0,heaps=0,mipped=0;
    for(const auto& [key,o]:s.objects){(void)key;if(o.kind==Kind::Resource){++resources;if(o.resource.MipLevels>1)++mipped;}if(o.kind==Kind::Heap)++heaps;}
    out<<"{\"unsupported_api_calls\":"<<s.unsupported_calls.load()<<",\"objects_alive\":"<<s.objects.size()<<",\"resources_alive\":"<<resources<<",\"mipped_resources_alive\":"<<mipped<<",\"heaps_alive\":"<<heaps<<",\"objects_created\":"<<s.created<<",\"objects_retired\":"<<s.retired<<",\"null_descriptors\":"<<s.ledger.null_count()<<",\"descriptor_slot_bytes\":"<<s.ledger.slot_count()*4<<",\"interned_descriptor_values\":"<<s.ledger.value_count()<<",\"orphan_descriptors\":"<<s.ledger.orphan_count()<<",\"descriptors_alive\":"<<s.ledger.known_count()-s.ledger.null_count()<<",\"untracked_heap_writes\":"<<s.untracked_heap_writes<<",\"descriptor_writes\":"<<s.descriptor_writes<<",\"command_records\":"<<s.commands.size()<<",\"captured_work\":"<<s.captured<<",\"captured_submissions\":"<<s.submitted<<",\"unknown_tables\":"<<s.unknown_tables<<",\"errors\":"<<s.errors<<",\"capturing\":"<<(s.capturing?"true":"false")<<",\"mutation_capability\":false,\"resource_records\":[";
    std::size_t exported=0;for(const auto& [address,o]:s.objects){(void)address;if(o.kind!=Kind::Resource)continue;if(exported>=64)break;if(exported++)out<<',';out<<"{\"id\":"<<o.id<<",\"width\":"<<o.resource.Width<<",\"height\":"<<o.resource.Height<<",\"mips\":"<<o.resource.MipLevels<<",\"format\":"<<o.resource.Format<<",\"flags\":"<<o.resource.Flags<<'}';}
    out<<"],\"resource_records_truncated\":"<<(resources>exported?"true":"false")<<",\"coverage_gap_sites\":[";
    bool first=true;for(const auto& [line,site]:s.error_sites){if(!first)out<<',';first=false;out<<"{\"line\":"<<line<<",\"where\":"<<std::quoted(site.function)<<",\"count\":"<<site.count<<'}';}out<<"],\"images_completed\":"<<s.images_completed<<",\"images_failed\":"<<s.images_failed<<",\"image_pending\":"<<(s.image_pending?"true":"false")<<",\"image_inflight\":"<<(s.image?"true":"false")<<'}';
}
}
