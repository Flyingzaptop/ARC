#include "arc/dx12_observer.hpp"
#include "arc/session.hpp"
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) { if (FAILED(hr)) { throw std::runtime_error("HRESULT " + std::to_string(static_cast<unsigned>(hr))); } }
static D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return d;
}
int main() try {
    if (std::getenv("ARC_D3D12_DEBUG")) { ComPtr<ID3D12Debug> debug; check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer(); }
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options))) || !options.EnhancedBarriersSupported) { std::cout << "Enhanced barriers unsupported\n"; return 77; }
    arc::Session session("enhanced-test.arcbin"); arc::IdAllocator ids; arc::dx12::Observer observer(session.ring(), ids);
    std::vector<std::pair<arc::ResourceId, ComPtr<ID3D12Resource>>> resources;
    auto create = [&](D3D12_RESOURCE_DESC d, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type; ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
        const auto id = observer.observe_committed_resource(device.Get(), d, r.Get()); if (!id) { throw std::runtime_error("observation overflow"); }
        resources.emplace_back(id, std::move(r)); return resources.size() - 1;
    };
    const auto upload = create(buffer(8192), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const auto readback = create(buffer(8192), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_DESC texture{}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width = texture.Height = 32;
    texture.DepthOrArraySize = texture.MipLevels = texture.SampleDesc.Count = 1; texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    const auto image = create(texture, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    void* data{}; D3D12_RANGE empty{}; check(resources[upload].second->Map(0, &empty, &data)); std::memset(data, 0xA5, 8192); resources[upload].second->Unmap(0, nullptr);
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{}; check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList7> list; check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    const auto command = ids.next(), queueId = ids.next();
    if (!observer.observe(arc::EventType::CommandQueueCreated, arc::QueueCreatePayload{.queue = queueId, .type = arc::QueueClass::Graphics}) ||
        !observer.observe(arc::EventType::CommandListCreated, arc::CommandListPayload{.command = command})) { return 1; }
    D3D12_TEXTURE_BARRIER b{}; b.SyncBefore = D3D12_BARRIER_SYNC_NONE; b.SyncAfter = D3D12_BARRIER_SYNC_COPY;
    b.AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS; b.AccessAfter = D3D12_BARRIER_ACCESS_COPY_DEST;
    b.LayoutBefore = D3D12_BARRIER_LAYOUT_COMMON; b.LayoutAfter = D3D12_BARRIER_LAYOUT_COPY_DEST; b.pResource = resources[image].second.Get(); b.Subresources = {0, 1, 0, 1, 0, 1};
    D3D12_BARRIER_GROUP group{}; group.Type = D3D12_BARRIER_TYPE_TEXTURE; group.NumBarriers = 1; group.pTextureBarriers = &b;
    list->Barrier(1, &group); if (!observer.observe_barrier(command, resources[image].first, b)) { return 1; }
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = resources[upload].second.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint.Footprint = {texture.Format, 32, 32, 1, 256};
    destination.pResource = resources[image].second.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (!observer.observe(arc::EventType::CopyTexture, arc::CopyPayload{.source = resources[upload].first, .destination = resources[image].first, .command = command, .approximate_bytes = 4096})) { return 1; }
    b.SyncBefore = D3D12_BARRIER_SYNC_COPY; b.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST; b.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
    b.LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_DEST; b.LayoutAfter = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    list->Barrier(1, &group); if (!observer.observe_barrier(command, resources[image].first, b)) { return 1; }
    source = destination; destination = {}; destination.pResource = resources[readback].second.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint.Footprint = {texture.Format, 32, 32, 1, 256};
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (!observer.observe(arc::EventType::CopyTexture, arc::CopyPayload{.source = resources[image].first, .destination = resources[readback].first, .command = command, .approximate_bytes = 4096})) { return 1; }
    check(list->Close()); if (!observer.observe(arc::EventType::CommandListClosed, arc::CommandListPayload{.command = command})) { return 1; }
    ID3D12CommandList* lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists);
    if (!observer.observe(arc::EventType::QueueSubmit, arc::QueueSubmitPayload{.queue = queueId, .command = command, .submission = 1})) { return 1; }
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))); check(queue->Signal(fence.Get(), 1));
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!done) { throw std::runtime_error("CreateEvent failed"); }
    check(fence->SetEventOnCompletion(1, done)); const auto wait = WaitForSingleObject(done, 10000); CloseHandle(done); if (wait != WAIT_OBJECT_0) { throw std::runtime_error("GPU wait failed"); }
    D3D12_RANGE range{0, 8192}; check(resources[readback].second->Map(0, &range, &data));
    bool valid = true; for (unsigned y = 0; y < 32; ++y) { for (unsigned x = 0; x < 128; ++x) { valid = valid && static_cast<unsigned char*>(data)[y * 256 + x] == 0xA5; } }
    resources[readback].second->Unmap(0, &empty);
    for (auto& [id, r] : resources) { r.Reset(); observer.observe_resource_destroyed(id); }
    session.finish(); valid = valid && session.complete() && session.graph().errors() == 0 && session.graph().copies().size() == 2;
    if (std::getenv("ARC_D3D12_DEBUG")) {
        ComPtr<ID3D12InfoQueue> info; check(device.As(&info));
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T size{}; check(info->GetMessage(i, nullptr, &size)); std::vector<std::byte> bytes(size);
            auto message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); check(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::cerr << message->pDescription << '\n'; valid = false; }
        }
    }
    std::cout << "Enhanced texture barriers: valid=" << valid << '\n'; return valid ? 0 : 1;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
