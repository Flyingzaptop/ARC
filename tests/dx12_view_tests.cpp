#include "arc/dx12_observer.hpp"
#include <iostream>
int main() {
    arc::EventRing ring(16); arc::IdAllocator ids; arc::dx12::Observer observer(ring, ids);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MostDetailedMip = 2; srv.Texture2DArray.MipLevels = UINT_MAX;
    srv.Texture2DArray.FirstArraySlice = 3; srv.Texture2DArray.ArraySize = 5;
    if (!observer.observe_srv(1, 2, srv)) { return 1; }
    if (!observer.observe_cbv(3, 4, 256, 1024) || !observer.observe_sampler(5)) { return 1; }
    arc::ResourceGraph g; arc::Event e{}; while (ring.try_pop(e)) { g.consume(e); }
    const auto view = g.find_view(1);
    if (!view || view->description.resource != 2 || view->description.first_mip != 2 || view->description.mip_count != UINT_MAX || view->description.first_layer != 3 || view->description.layer_count != 5) { std::cerr << "SRV range mismatch\n"; return 1; }
    if (g.find_view(3)->description.buffer_bytes != 1024 || g.find_view(5)->description.type != arc::ViewType::Sampler) { return 1; }
    D3D12_TEXTURE_BARRIER b{}; b.SyncBefore = D3D12_BARRIER_SYNC_COPY; b.SyncAfter = D3D12_BARRIER_SYNC_PIXEL_SHADING;
    b.LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_DEST; b.LayoutAfter = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    b.Subresources = {2, 3, 4, 5, 0, 1};
    if (!observer.observe_barrier(7, 8, b) || !ring.try_pop(e)) { return 1; }
    arc::ExtendedBarrierPayload p{}; std::memcpy(&p, e.payload.data(), sizeof(p));
    if (p.resource != 8 || p.command != 7 || p.first_mip != 2 || p.mip_count != 3 || p.first_layer != 4 || p.layer_count != 5 || p.layout_after != static_cast<unsigned>(D3D12_BARRIER_LAYOUT_SHADER_RESOURCE)) { return 1; }
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.NumDescriptors = 8;
    const auto heapId = observer.observe_descriptor_heap(heap, 32);
    if (!observer.observe(arc::EventType::DescriptorLocation, arc::DescriptorLocationPayload{.descriptor = 1, .heap = heapId})) { return 1; }
    if (!observer.observe(arc::EventType::DescriptorCopied, arc::DescriptorCopyPayload{.source = 1, .destination = 9})) { return 1; }
    observer.observe_descriptor_heap_destroyed(heapId);
    while (ring.try_pop(e)) { g.consume(e); }
    if (g.find_view(1)->alive || g.find_view(9)->description.mip_count != UINT_MAX || g.errors()) { return 1; }
    return 0;
}
