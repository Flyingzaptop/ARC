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
    return 0;
}
