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

    D3D12_HEAP_DESC placed_heap_desc{};
    placed_heap_desc.SizeInBytes = 16ULL * 1024ULL * 1024ULL;
    placed_heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    placed_heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> placed_heap;
    arc::HeapId placed_heap_id{};
    if (SUCCEEDED(device->CreateHeap(&placed_heap_desc, IID_PPV_ARGS(&placed_heap)))) {
        placed_heap_id = observer.observe_heap(placed_heap_desc, placed_heap.Get());
        D3D12_RESOURCE_DESC placed_desc{};
        placed_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        placed_desc.Width = 4ULL * 1024ULL * 1024ULL;
        placed_desc.Height = 1;
        placed_desc.DepthOrArraySize = 1;
        placed_desc.MipLevels = 1;
        placed_desc.SampleDesc.Count = 1;
        placed_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> placed_resource;
        if (SUCCEEDED(device->CreatePlacedResource(placed_heap.Get(), 0, &placed_desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&placed_resource)))) {
            resources.emplace_back(observer.observe_placed_resource(
                device.Get(), placed_heap_id, 0, placed_desc, placed_resource.Get()), placed_resource);
        }
    }

    arc::ResourceGraph graph;
    arc::Event event{};
    while (events.try_pop(event)) { graph.consume(event); }
    std::cout << "Observed " << graph.resource_count() << " resources; resource footprints: "
              << graph.live_allocation_bytes() / (1024 * 1024) << " MiB; heap allocations: "
              << graph.live_heap_bytes() / (1024 * 1024) << " MiB\n";
    for (const auto& [id, resource] : resources) { observer.observe_resource_destroyed(id); }
    if (placed_heap_id != 0) { observer.observe_heap_destroyed(placed_heap_id); }
    return 0;
}
