#ifdef _WIN32

#include "arc/dx12_host_adapter.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cassert>
#include <cstdint>
#include <iostream>

using Microsoft::WRL::ComPtr;

namespace {

void check(HRESULT hr) { assert(SUCCEEDED(hr)); }

struct Context {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<IDXGIAdapter3> adapter3;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;

    Context() {
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        check(warp.As(&adapter3));
        check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list)));
    }
};

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, std::uint64_t bytes) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> out;
    check(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&out)));
    return out;
}

} // namespace

int main() {
    Context c;
    arc::dx12::NativeHostAdapterConfig config{};
    config.runtime.coordinator.mode = arc::RuntimeMode::ObserveOnly;
    arc::dx12::NativeHostAdapter host(c.device.Get(), c.adapter3.Get(), config);

    const auto command = host.observe_command_list(c.list.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    assert(command != 0);

    auto a = buffer(c.device.Get(), 4096);
    auto b = buffer(c.device.Get(), 4096);
    const auto aid = host.observe_committed_resource(a.Get());
    const auto bid = host.observe_committed_resource(b.Get());
    assert(aid && bid);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    ComPtr<ID3D12DescriptorHeap> heap;
    check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    assert(host.observe_descriptor_heap(heap.Get()) != 0);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
    cbv.BufferLocation = a->GetGPUVirtualAddress();
    cbv.SizeInBytes = 256;
    const auto increment = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu0 = heap->GetCPUDescriptorHandleForHeapStart();
    auto cpu1 = cpu0;
    cpu1.ptr += increment;
    c.device->CreateConstantBufferView(&cbv, cpu0);
    assert(host.observe_cbv(heap.Get(), 0, a.Get(), 0, 256));
    c.device->CopyDescriptorsSimple(1, cpu1, cpu0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    assert(host.observe_descriptor_copy(heap.Get(), 0, heap.Get(), 1));

    D3D12_GLOBAL_BARRIER global{};
    global.SyncBefore = D3D12_BARRIER_SYNC_ALL;
    global.SyncAfter = D3D12_BARRIER_SYNC_ALL;
    global.AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
    global.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
    assert(host.observe_global_barrier(c.list.Get(), global));

    D3D12_BUFFER_BARRIER buffer_barrier{};
    buffer_barrier.SyncBefore = D3D12_BARRIER_SYNC_COPY;
    buffer_barrier.SyncAfter = D3D12_BARRIER_SYNC_COPY;
    buffer_barrier.AccessBefore = D3D12_BARRIER_ACCESS_COPY_SOURCE;
    buffer_barrier.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
    buffer_barrier.pResource = a.Get();
    buffer_barrier.Offset = 0;
    buffer_barrier.Size = 4096;
    assert(host.observe_buffer_barrier(c.list.Get(), a.Get(), buffer_barrier));

    assert(host.observe_copy_buffer(c.list.Get(), a.Get(), b.Get(), 4096));
    assert(host.observe_resource_use(c.list.Get(), a.Get(), false));
    assert(host.observe_resource_use(c.list.Get(), b.Get(), true));
    check(c.list->Close());
    assert(host.observe_command_list_closed(c.list.Get()));
    assert(host.drain() > 0);

    const auto copied = host.descriptor_id(heap.Get(), 1);
    assert(copied != 0);
    const auto copied_view = host.graph().find_view(copied);
    assert(copied_view.has_value());
    assert(copied_view->description.type == arc::ViewType::Cbv);
    assert(copied_view->description.resource == aid);
    assert(host.graph().errors() == 0);

    const auto metrics = host.metrics();
    assert(metrics.descriptor_writes == 1);
    assert(metrics.descriptor_copies == 1);
    assert(metrics.barriers_observed == 2);
    assert(metrics.copies_observed == 1);
    assert(metrics.failed_observations == 0);

    std::cout << "dx12-host-adapter-surface-tests: PASS\n";
    return 0;
}

#else
int main() { return 77; }
#endif
