#include "arc/dx12_observer.hpp"
#include "arc/dx12_residency.hpp"
#include "arc/residency.hpp"
#include "arc/session.hpp"
#include "arc/statistics.hpp"
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <vector>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) { if (FAILED(hr)) { throw std::runtime_error("HRESULT " + std::to_string(static_cast<unsigned>(hr))); } }
static D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return d;
}
struct Object { arc::ResourceId resource{}; arc::ResidencyId residency{}; ComPtr<ID3D12Resource> object; bool evicted{}, pending{}; std::uint64_t evicted_epoch{}, residency_fence{}; ID3D12Fence* residency_fence_object{}; };
int main(int argc, char** argv) try {
    const bool controlled = argc < 2 || std::string(argv[1]) == "arc";
    const unsigned count = argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 24;
    const UINT64 bytes = argc > 3 ? std::stoull(argv[3]) * 1024 * 1024 : 8ULL * 1024 * 1024;
    if (count < 8 || count > 128 || bytes < 65536 || bytes > 64ULL * 1024 * 1024) { throw std::runtime_error("invalid count/size"); }
    if (std::getenv("ARC_D3D12_DEBUG")) { ComPtr<ID3D12Debug> debug; check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer(); }
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; if (std::getenv("ARC_D3D12_DEBUG")) { check(device.As(&info)); }
    ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter3> adapter; check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
    arc::dx12::BudgetNotification notification(adapter.Get()); if (!notification.valid()) { throw std::runtime_error("budget notification unavailable"); }
    arc::dx12::ResidencyBackend residencyBackend(device.Get()); arc::ResidencyGovernor governor;
    std::filesystem::create_directories("traces");
    const std::string stem = controlled ? "residency-lab-arc" : "residency-lab-baseline";
    arc::Session session("traces/" + stem + ".arcbin", 65536); arc::IdAllocator ids; arc::dx12::Observer observer(session.ring(), ids);
    auto emit = [&](arc::EventType type, const auto& payload) { if (!observer.observe(type, payload)) { throw std::runtime_error("trace overflow"); } };
    auto budget = [&] { const auto value = arc::dx12::query_memory_budget(adapter.Get()); if (!value) { throw std::runtime_error("budget unavailable"); } emit(arc::EventType::MemoryBudgetSample, *value); return *value; };
    const auto before = budget();
    D3D12_HEAP_PROPERTIES uploadHeap{}; uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readbackHeap{}; readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> upload, readback; auto description = buffer(bytes);
    check(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    check(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    std::vector<Object> objects; objects.reserve(count);
    D3D12_HEAP_PROPERTIES defaultHeap{}; defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (unsigned index = 0; index < count; ++index) {
        Object object; check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&object.object)));
        object.resource = observer.observe_committed_resource(device.Get(), description, object.object.Get()); object.residency = ids.next();
        const auto safety = index + 2 < count ? arc::ResidencySafety::ControlledSafe : index + 1 == count ? arc::ResidencySafety::Unknown : arc::ResidencySafety::Pinned;
        if (!governor.register_object({.id = object.residency, .resource = object.resource, .state = arc::ResidencyState::Resident, .safety = safety, .cost = {.bytes = bytes}})) { throw std::runtime_error("registration failed"); }
        objects.push_back(std::move(object));
    }
    const auto allocated = budget();
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC queueDesc{}; check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands; check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!ready) { throw std::runtime_error("event creation failed"); }
    const auto queueId = ids.next(), commandId = ids.next(); std::uint64_t fenceValue{};
    emit(arc::EventType::CommandQueueCreated, arc::QueueCreatePayload{.queue = queueId, .type = arc::QueueClass::Graphics});
    emit(arc::EventType::CommandListCreated, arc::CommandListPayload{.command = commandId});
    auto execute = [&] {
        check(commands->Close()); emit(arc::EventType::CommandListClosed, arc::CommandListPayload{.command = commandId});
        ID3D12CommandList* lists[] = {commands.Get()}; queue->ExecuteCommandLists(1, lists);
        emit(arc::EventType::QueueSubmit, arc::QueueSubmitPayload{.queue = queueId, .command = commandId, .submission = fenceValue + 1});
        check(queue->Signal(fence.Get(), ++fenceValue)); check(fence->SetEventOnCompletion(fenceValue, ready));
        if (WaitForSingleObject(ready, 30000) != WAIT_OBJECT_0) { throw std::runtime_error("GPU timeout"); }
    };
    auto reset = [&] { check(allocator->Reset()); check(commands->Reset(allocator.Get(), nullptr)); emit(arc::EventType::CommandListReset, arc::CommandListPayload{.command = commandId}); };
    for (unsigned index = 0; index < count; ++index) {
        void* mapped{}; D3D12_RANGE empty{}; check(upload->Map(0, &empty, &mapped)); std::memset(mapped, static_cast<int>(index + 1), bytes); upload->Unmap(0, nullptr);
        if (index) { reset(); }
        commands->CopyResource(objects[index].object.Get(), upload.Get());
        emit(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command = commandId, .resource = objects[index].resource, .write = 1});
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.pResource = objects[index].object.Get();
        barrier.Transition.Subresource = UINT_MAX; barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; commands->ResourceBarrier(1, &barrier);
        emit(arc::EventType::Barrier, arc::BarrierPayload{.resource = objects[index].resource, .command = commandId, .before_state = D3D12_RESOURCE_STATE_COPY_DEST, .after_state = D3D12_RESOURCE_STATE_COPY_SOURCE, .subresource = UINT_MAX});
        execute(); governor.note_use(objects[index].residency, index + 1, fenceValue);
    }
    if (residencyBackend.evict_after(objects[0].object.Get(), fence.Get(), fenceValue + 1) != arc::dx12::ResidencyResult::UnsafeInFlight) { throw std::runtime_error("unsafe eviction was not rejected"); }
    std::uint64_t notificationCount{};
    if (auto notified = notification.wait(100)) {
        ++notificationCount;
        governor.update_budget(notified->local_budget, notified->local_usage);
    }
    std::uint64_t bytesEvicted{}, bytesResident{}, reloads{}, useful{}, falseEvictions{}, late{};
    std::vector<unsigned> sequence; sequence.reserve(2000);
    for (unsigned epoch = 0; epoch < 2000; ++epoch) {
        sequence.push_back(epoch % 200 == 150 ? 4 + (epoch / 200) % (count - 6) : epoch % 4);
    }
    // Eight epochs cover the deliberate one-frame cold insertion, preventing
    // the hot resource it displaces from being falsely classified as cold.
    constexpr std::size_t lookahead = 8;
    if (controlled) {
        governor.update_budget(100, 96);
        const auto planned = governor.plan(count + 100);
        for (const auto& action : planned) {
            const auto found = std::find_if(objects.begin(), objects.end(), [&](const Object& value) { return value.residency == action.object; });
            if (found == objects.end()) { throw std::runtime_error("planned object missing"); }
            const auto index = static_cast<unsigned>(std::distance(objects.begin(), found));
            if (std::find(sequence.begin(), sequence.begin() + lookahead, index) != sequence.begin() + lookahead) { continue; }
            if (residencyBackend.evict_after(objects[index].object.Get(), fence.Get(), fenceValue) != arc::dx12::ResidencyResult::Success) { throw std::runtime_error("Evict failed"); }
            if (!governor.transition(objects[index].residency, arc::ResidencyState::Resident, arc::ResidencyState::Evicted)) { throw std::runtime_error("invalid governor eviction"); }
            objects[index].evicted = true; objects[index].evicted_epoch = 0; bytesEvicted += bytes; ++useful;
            emit(arc::EventType::ResidencyTransition, arc::ResidencyTransitionPayload{.object = objects[index].residency, .before = arc::ResidencyState::Resident, .after = arc::ResidencyState::Evicted, .fence_value = fenceValue, .bytes = bytes});
        }
    }
    const auto afterEviction = budget();
    auto prefetch = [&](unsigned index, std::uint64_t epoch) {
        auto& object = objects[index];
        if (!object.evicted) { return; }
        if (!governor.transition(object.residency, arc::ResidencyState::Evicted, arc::ResidencyState::PendingResident)) { throw std::runtime_error("pending transition failed"); }
        const auto ticket = residencyBackend.enqueue_make_resident(object.object.Get());
        if (ticket.result != arc::dx12::ResidencyResult::Success) { ++late; throw std::runtime_error("asynchronous MakeResident failed"); }
        falseEvictions += epoch <= object.evicted_epoch + 4; object.evicted = false; object.pending = true;
        object.residency_fence = ticket.value; object.residency_fence_object = ticket.fence; bytesResident += bytes; ++reloads;
        emit(arc::EventType::ResidencyTransition, arc::ResidencyTransitionPayload{.object = object.residency, .before = arc::ResidencyState::Evicted, .after = arc::ResidencyState::PendingResident, .fence_value = ticket.value, .bytes = bytes});
    };
    if (controlled) { for (std::size_t future = 0; future < (std::min)(lookahead, sequence.size()); ++future) { prefetch(sequence[future], 0); } }
    std::vector<double> times; bool contents = true;
    for (std::size_t epoch = 0; epoch < sequence.size(); ++epoch) {
        const auto index = sequence[epoch]; const auto start = std::chrono::steady_clock::now();
        if (objects[index].evicted) {
            ++late; prefetch(index, epoch);
        }
        if (objects[index].pending) {
            // The queue wait inserted by prefetch guarantees residency before
            // this object's following GPU command; no CPU wait is required.
            check(queue->Wait(objects[index].residency_fence_object, objects[index].residency_fence));
        } else if (controlled && index + 2 < count && governor.find(objects[index].residency)->state != arc::ResidencyState::Resident) {
            throw std::runtime_error("object is not resident before use");
        }
        reset(); commands->CopyResource(readback.Get(), objects[index].object.Get());
        emit(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command = commandId, .resource = objects[index].resource}); execute();
        if (objects[index].pending) {
            if (!governor.transition(objects[index].residency, arc::ResidencyState::PendingResident, arc::ResidencyState::Resident)) { throw std::runtime_error("resident transition failed"); }
            objects[index].pending = false;
            emit(arc::EventType::ResidencyTransition, arc::ResidencyTransitionPayload{.object = objects[index].residency, .before = arc::ResidencyState::PendingResident, .after = arc::ResidencyState::Resident, .fence_value = fenceValue, .bytes = bytes});
        }
        void* mapped{}; D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)}; check(readback->Map(0, &range, &mapped));
        for (UINT64 offset = 0; offset < bytes; ++offset) { if (static_cast<unsigned char*>(mapped)[offset] != index + 1) { contents = false; break; } }
        D3D12_RANGE empty{}; readback->Unmap(0, &empty); governor.note_use(objects[index].residency, count + epoch + 1, fenceValue);
        const auto futureBegin = sequence.begin() + static_cast<std::ptrdiff_t>(epoch + 1);
        const auto futureEnd = sequence.begin() + static_cast<std::ptrdiff_t>((std::min)(sequence.size(), epoch + lookahead + 1));
        const bool neededSoon = std::find(futureBegin, futureEnd, index) != futureEnd;
        if (controlled && index + 2 < count && !neededSoon) {
            if (residencyBackend.evict_after(objects[index].object.Get(), fence.Get(), fenceValue) != arc::dx12::ResidencyResult::Success) { throw std::runtime_error("post-use Evict failed"); }
            if (!governor.transition(objects[index].residency, arc::ResidencyState::Resident, arc::ResidencyState::Evicted)) { throw std::runtime_error("post-use governor eviction failed"); }
            objects[index].evicted = true; objects[index].evicted_epoch = epoch; bytesEvicted += bytes; ++useful;
        }
        if (controlled && epoch + lookahead < sequence.size()) { prefetch(sequence[epoch + lookahead], epoch); }
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    for (auto& object : objects) {
        if (object.evicted) { if (residencyBackend.make_resident_and_wait(object.object.Get()) != arc::dx12::ResidencyResult::Success) { throw std::runtime_error("final MakeResident failed"); } object.evicted = false; bytesResident += bytes; ++reloads; }
        object.object.Reset(); observer.observe_resource_destroyed(object.resource);
    }
    CloseHandle(ready); session.finish(); const auto finalBudget = arc::dx12::query_memory_budget(adapter.Get());
    bool debugClean = true;
    if (info) { for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) { SIZE_T size{}; info->GetMessage(i, nullptr, &size); std::vector<std::byte> storage(size); auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); info->GetMessage(i, message, &size); if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::cerr << message->pDescription << '\n'; debugClean = false; } } }
    const auto stats = arc::summarize(times); const bool valid = contents && debugClean && session.complete() && session.graph().errors() == 0 && late == 0;
    std::ofstream report("traces/" + stem + ".json");
    report << "{\"schema\":1,\"mode\":\"" << (controlled ? "arc" : "baseline") << "\",\"valid\":" << (valid ? "true" : "false")
        << ",\"objects\":" << count << ",\"object_bytes\":" << bytes << ",\"target_resident_bytes\":" << 5 * bytes
        << ",\"contents_identical\":" << (contents ? "true" : "false") << ",\"bytes_evicted\":" << bytesEvicted << ",\"bytes_made_resident\":" << bytesResident
        << ",\"useful_evictions\":" << useful << ",\"false_evictions\":" << falseEvictions << ",\"reloads\":" << reloads << ",\"late_residency\":" << late
        << ",\"budget_notifications\":" << notificationCount << ",\"budget\":" << before.local_budget << ",\"usage_before\":" << before.local_usage
        << ",\"usage_allocated\":" << allocated.local_usage << ",\"usage_after_evict\":" << afterEviction.local_usage << ",\"usage_final\":" << (finalBudget ? finalBudget->local_usage : 0)
        << ",\"p50_ms\":" << stats.median << ",\"p95_ms\":" << stats.p95 << ",\"p99_ms\":" << stats.p99 << ",\"trace_drops\":" << session.dropped() << ",\"graph_errors\":" << session.graph().errors() << "}\n";
    std::cout << stem << ": valid=" << valid << " evicted_MiB=" << bytesEvicted / 1024 / 1024 << " reloads=" << reloads << " p99_ms=" << stats.p99 << '\n';
    return valid ? 0 : 1;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
