#include "arc/arc2/bridge.hpp"
#include <algorithm>
#include <cstring>
#include <map>

namespace arc::arc2 {
namespace {
template<class T> void emit(arc::ResourceGraph& graph,arc::EventType type,const T& payload,std::uint64_t sequence) {
    static_assert(sizeof(T)<=arc::kMaxEventPayloadBytes);
    arc::Event event{}; event.header.type=type; event.header.sequence=sequence; event.header.timestamp_ns=sequence; event.header.payload_bytes=sizeof(T);
    std::memcpy(event.payload.data(),&payload,sizeof(T)); graph.consume(event);
}
}
void feed_resource_graph(const IrSnapshot& ir,arc::ResourceGraph& graph) {
    std::uint64_t seq=1;
    std::map<ObjectId,ResourceDescription> resources;
    for(const auto& r:ir.resources) resources[r.resource]=r;
    for(const auto& obj:ir.objects) {
        if(obj.kind==ObjectKind::Heap) emit(graph,arc::EventType::HeapCreated,arc::HeapCreatePayload{obj.id.value},seq++);
        if(obj.kind==ObjectKind::Queue) emit(graph,arc::EventType::CommandQueueCreated,arc::QueueCreatePayload{obj.id.value},seq++);
        if(obj.kind==ObjectKind::Resource) {
            arc::ResourceCreatePayload p{}; p.resource=obj.id.value;
            auto it=resources.find(obj.id); if(it!=resources.end()) {
                const auto& r=it->second; p.heap=r.heap.value; p.virtual_bytes=r.bytes; p.allocation_bytes=r.bytes; p.heap_offset=r.offset;
                p.width=r.shape.width; p.height=r.shape.height; p.depth=r.shape.depth;
                p.array_layers=static_cast<std::uint16_t>(std::min(r.shape.array_layers,std::uint32_t(UINT16_MAX)));
                p.mip_levels=static_cast<std::uint16_t>(std::min(r.shape.mips,std::uint32_t(UINT16_MAX)));
                p.sample_count=r.shape.samples?r.shape.samples:1;
                p.format=r.shape.format_namespace==1?r.shape.format:0;
                p.resource_flags=r.shape.flags_namespace==1?r.shape.flags:0;
                switch(r.shape.dimension) { case ResourceDimension::Buffer:p.kind=arc::ResourceKind::Buffer;break;case ResourceDimension::Texture1D:p.kind=arc::ResourceKind::Texture1D;break;case ResourceDimension::Texture2D:p.kind=arc::ResourceKind::Texture2D;break;case ResourceDimension::Texture3D:p.kind=arc::ResourceKind::Texture3D;break;default:p.kind=arc::ResourceKind::Unknown; }
                switch(r.shape.allocation) { case ResourceAllocation::Committed:p.allocation_kind=arc::ResourceAllocationKind::Committed;break;case ResourceAllocation::Placed:p.allocation_kind=arc::ResourceAllocationKind::Placed;break;case ResourceAllocation::Reserved:p.allocation_kind=arc::ResourceAllocationKind::Reserved;break;case ResourceAllocation::External:p.allocation_kind=arc::ResourceAllocationKind::External;break;default:p.allocation_kind=r.heap?arc::ResourceAllocationKind::Placed:arc::ResourceAllocationKind::Committed; }
            }
            emit(graph,arc::EventType::ResourceCreated,p,seq++);
        }
    }
    std::map<std::uint64_t,const WorkItem*> work;
    for(const auto& w:ir.work) work[w->id.value]=w.get();
    for(const auto& sub:ir.submissions) {
        // A synthetic command per submission preserves list reuse and multiple
        // queue submissions without replaying mutable command-list state.
        const std::uint64_t command=sub.id.value;
        emit(graph,arc::EventType::CommandListCreated,arc::CommandListPayload{command},seq++);
        arc::CountersPayload counters{}; counters.command=command;
        for(auto wid:sub.work) {
            auto it=work.find(wid.value); if(it==work.end()) continue;
            const auto& item=*it->second;
            switch(item.kind) { case WorkKind::Draw: ++counters.draws; break; case WorkKind::DrawIndexed: ++counters.indexed_draws; break; case WorkKind::Dispatch: ++counters.dispatches; break; case WorkKind::ExecuteIndirect: ++counters.indirect; break; default: break; }
            if(sub.complete) for(const auto& a:item.accesses) if(a.certainty==Certainty::Known && a.kind!=AccessKind::Unknown && a.resource && resources.contains(a.resource)) emit(graph,arc::EventType::ResourceUse,arc::ResourceUsePayload{command,a.resource.value,a.kind==AccessKind::Write||a.kind==AccessKind::ReadWrite},seq++);
        }
        emit(graph,arc::EventType::CommandCounters,counters,seq++);
        emit(graph,arc::EventType::CommandListClosed,arc::CommandListPayload{command},seq++);
        emit(graph,arc::EventType::QueueSubmit,arc::QueueSubmitPayload{sub.queue.value,command,sub.id.value},seq++);
    }
    for(const auto& p:ir.presents) emit(graph,arc::EventType::Present,arc::PresentPayload{p.swapchain.value,p.sequence,0,0,p.result},seq++);
}
} // namespace arc::arc2
