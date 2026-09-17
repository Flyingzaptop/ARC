#include "arc/dx12_observer.hpp"
#include "arc/live_runtime.hpp"
#include "arc/runtime_event_bridge.hpp"

#include <wrl/client.h>
#include <d3d12sdklayers.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace {

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(std::string(what) + " HRESULT=" + std::to_string(static_cast<unsigned>(hr)));
}

D3D12_RESOURCE_DESC buffer(std::uint64_t bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

}  // namespace

int main() try {
    constexpr std::uint64_t bytes = 4ull * 1024ull * 1024ull;

    if (std::getenv("ARC_D3D12_DEBUG")) {
        ComPtr<ID3D12Debug> debug;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) return 77;
        debug->EnableDebugLayer();
    }

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return 77;
    std::atomic<unsigned> debug_errors{};
    ComPtr<ID3D12InfoQueue1> info;
    DWORD callback_cookie{};
    if (std::getenv("ARC_D3D12_DEBUG") && SUCCEEDED(device.As(&info))) {
        check(info->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID, LPCSTR, void* context) {
                if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    static_cast<std::atomic<unsigned>*>(context)->fetch_add(1, std::memory_order_relaxed);
                }
            }, D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, &debug_errors, &callback_cookie), "RegisterMessageCallback");
    }
    struct CallbackGuard {
        ID3D12InfoQueue1* queue{}; DWORD cookie{};
        ~CallbackGuard() { if (queue && cookie) queue->UnregisterMessageCallback(cookie); }
    } callback_guard{info.Get(), callback_cookie};

    arc::EventRing ring(512);
    arc::IdAllocator ids;
    arc::dx12::Observer observer(ring, ids);
    arc::LiveRuntimeConfig config{};
    config.residency.minimum_residency_age_epochs = 0;
    config.residency.recovery_samples = 1;
    arc::LiveRuntimeController runtime(config);
    arc::RuntimeEventBridge bridge(runtime);

    auto drain = [&] {
        arc::Event event{};
        bool ok = true;
        while (ring.try_pop(event)) ok = bridge.consume(event) && ok;
        return ok;
    };

    D3D12_HEAP_PROPERTIES upload_heap{}; upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES default_heap{}; default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readback_heap{}; readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    const auto desc = buffer(bytes);
    ComPtr<ID3D12Resource> upload, controlled, unknown, readback;
    check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "Create upload");
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&controlled)), "Create controlled");
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&unknown)), "Create unknown");
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create readback");

    const auto controlled_id = observer.observe_committed_resource(device.Get(), desc, controlled.Get());
    const auto unknown_id = observer.observe_committed_resource(device.Get(), desc, unknown.Get());
    if (!controlled_id || !unknown_id || !drain()) throw std::runtime_error("resource observation failed");
    constexpr arc::ResidencyId residency_id = 9001;
    if (!runtime.register_controlled_resource({
        .id=residency_id,.resource=controlled_id,.state=arc::ResidencyState::Resident,
        .safety=arc::ResidencySafety::ControlledSafe,.cost={.bytes=bytes,.reload_ms=.2}})) {
        throw std::runtime_error("controlled registration failed");
    }

    void* mapped{};
    D3D12_RANGE empty{};
    check(upload->Map(0, &empty, &mapped), "Upload Map");
    std::memset(mapped, 0x5a, static_cast<std::size_t>(bytes));
    upload->Unmap(0, nullptr);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "Create queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Create list");
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create fence");
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!done) throw std::runtime_error("CreateEvent failed");
    struct EventGuard { HANDLE value{}; ~EventGuard(){ if (value) CloseHandle(value); } } event_guard{done};

    const auto queue_id = ids.next();
    const auto command_id = ids.next();
    const auto fence_id = ids.next();
    if (!observer.observe(arc::EventType::CommandQueueCreated, arc::QueueCreatePayload{.queue=queue_id,.type=arc::QueueClass::Graphics}) ||
        !observer.observe(arc::EventType::CommandListCreated, arc::CommandListPayload{.command=command_id,.type=arc::QueueClass::Graphics})) {
        throw std::runtime_error("initial event emission failed");
    }

    list->CopyResource(controlled.Get(), upload.Get());
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = controlled.Get();
    barrier.Transition.Subresource = UINT_MAX;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    list->CopyResource(readback.Get(), controlled.Get());
    check(list->Close(), "Close list");
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), 1), "Signal fence");

    // Emit the same lifecycle that a real adapter reports. The bridge consumes
    // it before CPU fence completion is acknowledged.
    if (!observer.observe(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command=command_id,.resource=controlled_id}) ||
        !observer.observe(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command=command_id,.resource=unknown_id}) ||
        !observer.observe(arc::EventType::CommandListClosed, arc::CommandListPayload{.command=command_id,.type=arc::QueueClass::Graphics}) ||
        !observer.observe(arc::EventType::QueueSubmit, arc::QueueSubmitPayload{.queue=queue_id,.command=command_id,.submission=1}) ||
        !observer.observe(arc::EventType::FenceSignal, arc::FencePayload{.queue=queue_id,.fence=fence_id,.value=1})) {
        throw std::runtime_error("use event emission failed");
    }
    if (!drain()) throw std::runtime_error("bridge rejected valid event stream");

    arc::MemoryBudgetPayload pressure{};
    pressure.local_budget = 1000;
    pressure.local_usage = 900;
    if (!observer.observe(arc::EventType::MemoryBudgetSample, pressure) || !drain()) {
        throw std::runtime_error("budget event failed");
    }
    const auto before_completion = runtime.plan(100);
    const auto before_actions = before_completion.pressure_relief.arbitration.actions.size();

    check(fence->SetEventOnCompletion(1, done), "SetEventOnCompletion");
    if (WaitForSingleObject(done, 30000) != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
    bridge.note_queue_completed(queue_id, fence->GetCompletedValue());
    const auto after_completion = runtime.plan(101);
    const auto after_actions = after_completion.pressure_relief.arbitration.actions.size();

    void* read{};
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    check(readback->Map(0, &range, &read), "Readback Map");
    const auto* data = static_cast<const unsigned char*>(read);
    bool contents = data[0] == 0x5a && data[bytes - 1] == 0x5a;
    readback->Unmap(0, &empty);

    observer.observe_resource_destroyed(controlled_id);
    if (!drain()) throw std::runtime_error("destroy event failed");
    const bool destroyed_unregistered = !runtime.controlled(controlled_id);
    const bool unknown_uncontrolled = !runtime.controlled(unknown_id);
    const auto metrics = bridge.metrics();
    const bool valid = before_actions == 0 && after_actions > 0 && contents && destroyed_unregistered &&
        unknown_uncontrolled && metrics.malformed_events == 0 && metrics.controller_rejections == 0 &&
        ring.dropped_events() == 0 && debug_errors.load(std::memory_order_relaxed) == 0;

    std::filesystem::create_directories("traces");
    std::ofstream out("traces/runtime-event-bridge-lab.json", std::ios::trunc);
    out << "{\n"
        << "  \"schema\": 1,\n"
        << "  \"valid\": " << (valid ? "true" : "false") << ",\n"
        << "  \"actions_before_completion\": " << before_actions << ",\n"
        << "  \"actions_after_completion\": " << after_actions << ",\n"
        << "  \"contents_verified\": " << (contents ? "true" : "false") << ",\n"
        << "  \"destroyed_resource_unregistered\": " << (destroyed_unregistered ? "true" : "false") << ",\n"
        << "  \"unknown_resource_uncontrolled\": " << (unknown_uncontrolled ? "true" : "false") << ",\n"
        << "  \"bridge_resource_uses\": " << metrics.resource_uses << ",\n"
        << "  \"bridge_malformed_events\": " << metrics.malformed_events << ",\n"
        << "  \"bridge_controller_rejections\": " << metrics.controller_rejections << ",\n"
        << "  \"ring_dropped_events\": " << ring.dropped_events() << ",\n"
        << "  \"debug_error_count\": " << debug_errors.load(std::memory_order_relaxed) << "\n"
        << "}\n";

    observer.observe_resource_destroyed(unknown_id);
    controlled.Reset();
    unknown.Reset();
    readback.Reset();
    upload.Reset();

    std::cout << "runtime-event-bridge valid=" << valid
              << " before=" << before_actions
              << " after=" << after_actions
              << " uses=" << metrics.resource_uses
              << " debug_errors=" << debug_errors.load(std::memory_order_relaxed) << '\n';
    return valid ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "runtime-event-bridge-lab: " << error.what() << '\n';
    return 1;
}
