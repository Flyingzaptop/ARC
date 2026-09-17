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

void check(HRESULT hr) {
    assert(SUCCEEDED(hr));
}

struct Context {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<IDXGIAdapter3> adapter3;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;

    Context() {
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        check(warp.As(&adapter3));
        check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }
};

ComPtr<ID3D12Resource> create_buffer(ID3D12Device* device, std::uint64_t bytes) {
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
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}

} // namespace

int main() {
    Context c;

    arc::dx12::NativeHostAdapterConfig config{};
    config.runtime.coordinator.mode = arc::RuntimeMode::PlanOnly;
    config.runtime.coordinator.max_budget_age_ticks = 0;
    config.runtime.runtime.residency.minimum_residency_age_epochs = 1;
    arc::dx12::NativeHostAdapter host(c.device.Get(), c.adapter3.Get(), config);
    host.set_mode(arc::RuntimeMode::PlanOnly);

    const auto qid = host.observe_queue(c.queue.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    const auto cid = host.observe_command_list(c.list.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    assert(qid != 0 && cid != 0);
    assert(host.observe_queue(c.queue.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT) == qid);
    assert(host.observe_command_list(c.list.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT) == cid);

    auto resource = create_buffer(c.device.Get(), 4ull << 20);
    const auto rid = host.observe_committed_resource(resource.Get());
    assert(rid != 0);
    assert(host.observe_committed_resource(resource.Get()) == rid);
    assert(host.resource_id(resource.Get()) == rid);

    D3D12_DESCRIPTOR_HEAP_DESC dh{};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dh.NumDescriptors = 4;
    ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    check(c.device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&descriptor_heap)));
    assert(host.observe_descriptor_heap(descriptor_heap.Get()) != 0);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32_UINT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Buffer.NumElements = static_cast<UINT>((4ull << 20) / 4);
    c.device->CreateShaderResourceView(resource.Get(), &srv, descriptor_heap->GetCPUDescriptorHandleForHeapStart());
    assert(host.observe_srv(descriptor_heap.Get(), 0, resource.Get(), srv));
    assert(host.descriptor_id(descriptor_heap.Get(), 0) != 0);

    // Explicit opt-in is required before the resource can participate in
    // residency mutation/safety tracking.
    assert(host.enable_residency_control(resource.Get(), 0.2));
    assert(host.runtime().runtime().controlled(rid));
    assert(host.backend().has_resource(rid));

    const auto fid = host.bind_completion_fence(c.queue.Get(), c.fence.Get());
    assert(fid != 0);
    assert(host.fence_id(c.fence.Get()) == fid);

    assert(host.observe_resource_use(c.list.Get(), resource.Get(), false));
    check(c.list->Close());
    assert(host.observe_command_list_closed(c.list.Get()));
    ID3D12CommandList* submitted[]{c.list.Get()};
    c.queue->ExecuteCommandLists(1, submitted);
    assert(host.observe_execute_command_lists(c.queue.Get(), submitted));
    check(c.queue->Signal(c.fence.Get(), 1));
    assert(host.observe_fence_signal(c.queue.Get(), c.fence.Get(), 1));
    assert(host.drain() > 0);
    assert(host.runtime().runtime().inflight_count(rid) == 1);

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    assert(event != nullptr);
    if (c.fence->GetCompletedValue() < 1) {
        check(c.fence->SetEventOnCompletion(1, event));
        assert(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
    }
    CloseHandle(event);
    assert(host.poll_completion(c.queue.Get()));
    assert(host.runtime().runtime().inflight_count(rid) == 0);

    assert(host.observe_present(1, 0, 0, S_OK));
    assert(host.drain() > 0);
    assert(host.graph().presentation_frame() == 1);
    const auto record = host.graph().find(rid);
    assert(record.has_value() && record->alive);
    assert(record->read_count >= 1);

    arc::FrameBudgetSample frame{};
    frame.frame_ms = 10.0;
    frame.target_frame_ms = 16.667;
    frame.local_budget_bytes = 512ull << 20;
    frame.local_usage_bytes = 256ull << 20;
    const auto tick = host.frame_tick(frame, false);
    assert(tick.path == arc::UnifiedGovernorPath::PlanOnly ||
           tick.path == arc::UnifiedGovernorPath::None);

    assert(host.observe_resource_destroyed(resource.Get()));
    assert(host.drain() > 0);
    assert(!host.runtime().runtime().controlled(rid));
    assert(!host.backend().has_resource(rid));
    const auto dead = host.graph().find(rid);
    assert(dead.has_value() && !dead->alive);

    const auto metrics = host.metrics();
    assert(metrics.resources_observed == 1);
    assert(metrics.resources_destroyed == 1);
    assert(metrics.queue_submits == 1);
    assert(metrics.fence_signals == 1);
    assert(metrics.completion_updates >= 1);
    assert(metrics.bridge_rejections == 0);
    assert(metrics.failed_observations == 0);

    std::cout << "dx12-host-adapter-tests: PASS\n";
    return 0;
}

#else
int main() { return 77; }
#endif
