#include "arc/dx12_observer.hpp"
#include "arc/resource_graph.hpp"

#include <wrl/client.h>

#include <d3d12.h>
#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::cerr << "No usable D3D12 device available\n";
        return 1;
    }

    arc::EventRing events(1024);
    arc::IdAllocator ids;
    arc::dx12::Observer observer(events, ids);
    std::vector<std::pair<arc::ResourceId, ComPtr<ID3D12Resource>>> resources;
    for (std::uint64_t bytes : {4ULL, 8ULL, 16ULL, 32ULL}) {
        D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes * 1024ULL * 1024ULL;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> resource;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)))) {
            std::cerr << "Resource allocation failed at " << bytes << " MiB\n";
            break;
        }
        resources.emplace_back(observer.observe_committed_resource(device.Get(), desc, resource.Get()), resource);
    }

    arc::ResourceGraph graph;
    arc::Event event{};
    while (events.try_pop(event)) { graph.consume(event); }
    std::cout << "Observed " << graph.resource_count() << " resources, "
              << graph.live_allocation_bytes() / (1024 * 1024) << " MiB allocated\n";
    for (const auto& [id, resource] : resources) { observer.observe_resource_destroyed(id); }
    return 0;
}
