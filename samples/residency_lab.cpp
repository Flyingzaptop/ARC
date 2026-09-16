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
enum class LabMode { Baseline, Oracle, Autonomous };
static const char* mode_name(LabMode mode) {
    switch (mode) { case LabMode::Baseline: return "baseline"; case LabMode::Oracle: return "arc"; case LabMode::Autonomous: return "auto"; }
    return "unknown";
}
struct Object { arc::ResourceId resource{}; arc::ResidencyId residency{}; ComPtr<ID3D12Resource> object; bool evicted{}, pending{}; std::uint64_t evicted_epoch{}, residency_fence{}; ID3D12Fence* residency_fence_object{}; };
int main(int argc, char** argv) try {
    const std::string requestedMode = argc > 1 ? argv[1] : "arc";
    const auto mode = requestedMode == "baseline" ? LabMode::Baseline : requestedMode == "arc" ? LabMode::Oracle : requestedMode == "auto" ? LabMode::Autonomous : throw std::runtime_error("mode must be baseline|arc|auto");
    const bool controlled = mode != LabMode::Baseline;
    const bool autonomous = mode == LabMode::Autonomous;
    const unsigned count = argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 24;
    const UINT64 bytes = argc > 3 ? std::stoull(argv[3]) * 1024 * 1024 : 8ULL * 1024 * 1024;
    if (count < 8 || count > 128 || bytes < 65536 || bytes > 64ULL * 1024 * 1024) { throw std::runtime_error("invalid count/size"); }
    if (std::getenv("ARC_D3D12_DEBUG")) { ComPtr<ID3D12Debug> debug; check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer(); }
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; if (std::getenv("ARC_D3D12_DEBUG")) { check(device.As(&info)); }
    ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter3> adapter; check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
    arc::dx12::BudgetNotification notification(adapter.Get()); if (!notification.valid()) { throw std::runtime_error("budget notification unavailable"); }
    arc::dx12::ResidencyBackend residencyBackend(device.Get());
    arc::ResidencyPolicyConfig governorConfig{};
    if (autonomous) { governorConfig.minimum_residency_age_epochs = 4; governorConfig.prefetch_horizon_epochs = 8; governorConfig.emergency_target = .72; governorConfig.pressure_target = .78; governorConfig.promotion_ceiling = .84; governorConfig.recovery_samples = 2; }
    arc::ResidencyGovernor governor(governorConfig);
    std::filesystem::create_directories("traces");
    const std::string stem = std::string("residency-lab-") + mode_name(mode);
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
        if (!governor.register_object({.id = object.residency, .resource = object.resource, .state = arc::ResidencyState::Resident, .safety = safety, .cost = {.bytes = bytes, .reload_ms = .25}})) { throw std::runtime_error("registration failed"); }
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
        execute(); governor.note_use(objects[index].residency, index + 1, queueId, fenceValue, fenceValue);
    }
    if (residencyBackend.evict_after(objects[0].object.Get(), fence.Get(), fenceValue + 1) != arc::dx12::ResidencyResult::UnsafeInFlight) { throw std::runtime_error("unsafe eviction was not rejected"); }
    std::uint64_t notificationCount{};
    if (auto notified = notification.wait(100)) { ++notificationCount; }
    std::uint64_t bytesEvicted{}, bytesResident{}, reloads{}, useful{}, falseEvictions{}, late{}, speculativePromotions{};
    std::uint64_t managedResidentBytes = static_cast<std::uint64_t>(count) * bytes;
    const unsigned managedTargetObjects = (std::min)(count, (std::max)(6U, count / 2));
    const std::uint64_t managedBudget = static_cast<std::uint64_t>(managedTargetObjects) * bytes;
    std::uint64_t managedPeak = managedResidentBytes, managedSum{};
    constexpr std::size_t lookahead = 8;
    constexpr std::size_t warmupEpochs = 400;
    constexpr std::size_t measuredEpochs = 2000;
    auto workload_index = [&](std::size_t epoch) -> unsigned { return epoch % 200 == 150 ? 4 + static_cast<unsigned>((epoch / 200) % (count - 6)) : epoch % 4; };
    auto find_index = [&](arc::ResidencyId residency) -> unsigned {
        const auto found = std::find_if(objects.begin(), objects.end(), [&](const Object& value) { return value.residency == residency; });
        if (found == objects.end()) { throw std::runtime_error("planned object missing"); }
        return static_cast<unsigned>(std::distance(objects.begin(), found));
    };
    auto evict = [&](unsigned index, std::uint64_t requiredFence, std::uint64_t epoch) {
        auto& object = objects[index]; if (object.evicted || object.pending) { return; }
        if (residencyBackend.evict_after(object.object.Get(), fence.Get(), requiredFence) != arc::dx12::ResidencyResult::Success) { throw std::runtime_error("Evict failed"); }
        if (!governor.transition(object.residency, arc::ResidencyState::Resident, arc::ResidencyState::Evicted)) { throw std::runtime_error("invalid governor eviction"); }
        object.evicted = true; object.evicted_epoch = epoch; bytesEvicted += bytes; ++useful;
        if (autonomous) { if (managedResidentBytes < bytes) { throw std::runtime_error("managed residency underflow"); } managedResidentBytes -= bytes; }
        emit(arc::EventType::ResidencyTransition, arc::ResidencyTransitionPayload{.object = object.residency, .before = arc::ResidencyState::Resident, .after = arc::ResidencyState::Evicted, .fence_value = requiredFence, .bytes = bytes});
    };
    auto prefetch = [&](unsigned index, std::uint64_t epoch, bool speculative) {
        auto& object = objects[index]; if (!object.evicted) { return; }
        if (!governor.require_resident(object.residency)) { throw std::runtime_error("resident demand rejected"); }
        if (!governor.transition(object.residency, arc::ResidencyState::Evicted, arc::ResidencyState::PendingResident)) { throw std::runtime_error("pending transition failed"); }
        const auto ticket = residencyBackend.enqueue_make_resident(object.object.Get());
        if (ticket.result != arc::dx12::ResidencyResult::Success) { throw std::runtime_error("asynchronous MakeResident failed"); }
        falseEvictions += epoch <= object.evicted_epoch + 4; object.evicted = false; object.pending = true;
        object.residency_fence = ticket.value; object.residency_fence_object = ticket.fence; bytesResident += bytes; ++reloads;
        if (speculative) { ++speculativePromotions; }
        if (autonomous) { managedResidentBytes += bytes; managedPeak = (std::max)(managedPeak, managedResidentBytes); }
        emit(arc::EventType::ResidencyTransition, arc::ResidencyTransitionPayload{.object = object.residency, .before = arc::ResidencyState::Evicted, .after = arc::ResidencyState::PendingResident, .fence_value = ticket.value, .bytes = bytes});
    };
    bool contents = true;
    auto use_object = [&](unsigned index, std::uint64_t logicalEpoch, bool measure, std::vector<double>& times) {
        const auto start = std::chrono::steady_clock::now();
        if (objects[index].evicted) { ++late; prefetch(index, logicalEpoch, false); }
        if (objects[index].pending) { check(queue->Wait(objects[index].residency_fence_object, objects[index].residency_fence)); }
        else if (controlled && index + 2 < count && governor.find(objects[index].residency)->state != arc::ResidencyState::Resident) { throw std::runtime_error("object is not resident before use"); }
        reset(); commands->CopyResource(readback.Get(), objects[index].object.Get());
        emit(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command = commandId, .resource = objects[index].resource}); execute();
        if (objects[index].pending) {
            if (!governor.transition(objects[index].residency, arc::ResidencyState::PendingResident, arc::ResidencyState::Resident)) { throw std::runtime_error("resident transition failed"); }
            objects[index].pending = false;
            emit(arc::EventType::ResidencyTransition, arc::ResidencyTransitionPayload{.object = objects[index].residency, .before = arc::ResidencyState::PendingResident, .after = arc::ResidencyState::Resident, .fence_value = fenceValue, .bytes = bytes});
        }
        void* mapped{}; D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)}; check(readback->Map(0, &range, &mapped));
        for (UINT64 offset = 0; offset < bytes; ++offset) { if (static_cast<unsigned char*>(mapped)[offset] != index + 1) { contents = false; break; } }
        D3D12_RANGE empty{}; readback->Unmap(0, &empty);
        governor.note_use(objects[index].residency, logicalEpoch, queueId, fenceValue, fence->GetCompletedValue());
        if (measure) { times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()); }
    };
    std::vector<double> times;
    if (autonomous) { for (std::size_t epoch = 0; epoch < warmupEpochs; ++epoch) { use_object(workload_index(epoch), count + epoch + 1, false, times); } }
    if (mode == LabMode::Oracle) {
        for (unsigned index = 0; index + 2 < count; ++index) {
            bool neededSoon = false;
            for (std::size_t future = 0; future < lookahead; ++future) { neededSoon |= workload_index(future) == index; }
            if (!neededSoon) { evict(index, governor.find(objects[index].residency)->last_use_fence, 0); }
        }
        for (std::size_t future = 0; future < lookahead; ++future) { prefetch(workload_index(future), 0, true); }
    }
    const auto afterInitialPolicy = budget();
    for (std::size_t epoch = 0; epoch < measuredEpochs; ++epoch) {
        const auto logicalEpoch = count + warmupEpochs + epoch + 1;
        if (autonomous) {
            governor.update_budget(managedBudget, managedResidentBytes);
            for (const auto& action : governor.plan_evictions(logicalEpoch)) {
                if (action.required_queue && action.required_queue != queueId) { throw std::runtime_error("unexpected eviction queue"); }
                evict(find_index(action.object), action.required_fence, logicalEpoch);
            }
            governor.update_budget(managedBudget, managedResidentBytes);
            for (const auto& action : governor.plan_promotions(logicalEpoch)) { prefetch(find_index(action.object), logicalEpoch, true); }
        }
        const auto index = workload_index(autonomous ? warmupEpochs + epoch : epoch);
        use_object(index, logicalEpoch, true, times);
        if (mode == LabMode::Oracle) {
            bool neededSoon = false;
            for (std::size_t future = epoch + 1; future < (std::min)(measuredEpochs, epoch + lookahead + 1); ++future) { neededSoon |= workload_index(future) == index; }
            if (index + 2 < count && !neededSoon) { evict(index, governor.find(objects[index].residency)->last_use_fence, epoch); }
            if (epoch + lookahead < measuredEpochs) { prefetch(workload_index(epoch + lookahead), epoch, true); }
        }
        if (autonomous) { managedPeak = (std::max)(managedPeak, managedResidentBytes); managedSum += managedResidentBytes; }
    }
    for (auto& object : objects) {
        if (object.evicted) {
            if (residencyBackend.make_resident_and_wait(object.object.Get()) != arc::dx12::ResidencyResult::Success) { throw std::runtime_error("final MakeResident failed"); }
            if (governor.transition(object.residency, arc::ResidencyState::Evicted, arc::ResidencyState::PendingResident)) { (void)governor.transition(object.residency, arc::ResidencyState::PendingResident, arc::ResidencyState::Resident); }
            object.evicted = false; bytesResident += bytes; ++reloads;
        }
        object.object.Reset(); observer.observe_resource_destroyed(object.resource);
    }
    CloseHandle(ready); session.finish(); const auto finalBudget = arc::dx12::query_memory_budget(adapter.Get());
    bool debugClean = true;
    if (info) { for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) { SIZE_T size{}; info->GetMessage(i, nullptr, &size); std::vector<std::byte> storage(size); auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); info->GetMessage(i, message, &size); if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::cerr << message->pDescription << '\n'; debugClean = false; } } }
    const auto stats = arc::summarize(times);
    const bool valid = contents && debugClean && session.complete() && session.graph().errors() == 0 && (mode != LabMode::Oracle || late == 0);
    const auto averageManaged = autonomous && measuredEpochs ? managedSum / measuredEpochs : 0;
    std::ofstream report("traces/" + stem + ".json");
    report << "{\"schema\":2,\"mode\":\"" << mode_name(mode) << "\",\"valid\":" << (valid ? "true" : "false")
        << ",\"objects\":" << count << ",\"object_bytes\":" << bytes << ",\"managed_budget\":" << (autonomous ? managedBudget : 0)
        << ",\"managed_usage_final\":" << (autonomous ? managedResidentBytes : 0) << ",\"managed_usage_average\":" << averageManaged << ",\"managed_usage_peak\":" << (autonomous ? managedPeak : 0)
        << ",\"contents_identical\":" << (contents ? "true" : "false") << ",\"bytes_evicted\":" << bytesEvicted << ",\"bytes_made_resident\":" << bytesResident
        << ",\"useful_evictions\":" << useful << ",\"false_evictions\":" << falseEvictions << ",\"reloads\":" << reloads << ",\"late_residency\":" << late << ",\"speculative_promotions\":" << speculativePromotions
        << ",\"budget_notifications\":" << notificationCount << ",\"budget\":" << before.local_budget << ",\"usage_before\":" << before.local_usage
        << ",\"usage_allocated\":" << allocated.local_usage << ",\"usage_after_initial_policy\":" << afterInitialPolicy.local_usage << ",\"usage_final\":" << (finalBudget ? finalBudget->local_usage : 0)
        << ",\"p50_ms\":" << stats.median << ",\"p95_ms\":" << stats.p95 << ",\"p99_ms\":" << stats.p99 << ",\"trace_drops\":" << session.dropped() << ",\"graph_errors\":" << session.graph().errors() << "}\n";
    std::cout << stem << ": valid=" << valid << " evicted_MiB=" << bytesEvicted / 1024 / 1024 << " reloads=" << reloads << " late=" << late << " speculative=" << speculativePromotions << " p99_ms=" << stats.p99 << '\n';
    return valid ? 0 : 1;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
