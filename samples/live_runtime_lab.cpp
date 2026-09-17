#include "arc/dx12_observer.hpp"
#include "arc/dx12_residency.hpp"
#include "arc/live_runtime.hpp"

#include <wrl/client.h>
#include <d3d12sdklayers.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

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

struct Object {
    ComPtr<ID3D12Resource> resource;
    arc::ResourceId resource_id{};
    arc::ResidencyId residency_id{};
    unsigned pattern{};
    bool controlled{};
};

}  // namespace

int main() try {
    constexpr std::uint64_t object_bytes = 8ull * 1024ull * 1024ull;
    constexpr unsigned controlled_count = 4;
    constexpr unsigned unknown_index = controlled_count;

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

    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter3> adapter;
    check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid");

    arc::EventRing ring(1024);
    arc::IdAllocator ids;
    arc::dx12::Observer observer(ring, ids);
    arc::dx12::ResidencyBackend backend(device.Get());

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
    check(list->Close(), "initial Close");
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!done) throw std::runtime_error("CreateEvent failed");
    struct EventGuard { HANDLE value{}; ~EventGuard(){ if (value) CloseHandle(value); } } event_guard{done};
    std::uint64_t fence_value{};

    auto reset = [&] {
        check(allocator->Reset(), "Allocator Reset");
        check(list->Reset(allocator.Get(), nullptr), "CommandList Reset");
    };
    auto execute = [&] {
        check(list->Close(), "CommandList Close");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), ++fence_value), "Queue Signal");
        check(fence->SetEventOnCompletion(fence_value, done), "Fence event");
        if (WaitForSingleObject(done, 30000) != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
    };

    D3D12_HEAP_PROPERTIES upload_heap{}; upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readback_heap{}; readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_HEAP_PROPERTIES default_heap{}; default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto desc = buffer(object_bytes);
    ComPtr<ID3D12Resource> upload, readback;
    check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "Create upload");
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create readback");

    arc::LiveRuntimeConfig runtime_config{};
    runtime_config.residency.minimum_residency_age_epochs = 0;
    runtime_config.residency.recovery_samples = 1;
    runtime_config.residency.minimum_prediction_samples = 3;
    runtime_config.residency.promotion_ceiling = 0.90;
    runtime_config.transitions.minimum_context_observations = 3;
    runtime_config.transitions.max_context_samples = 128;
    runtime_config.transitions.minimum_probability = 0.20;
    runtime_config.transitions.minimum_confidence = 0.20;
    runtime_config.minimum_transition_prefetch_confidence = 0.20;
    runtime_config.max_transition_prefetch_actions = 2;
    arc::LiveRuntimeController runtime(runtime_config);

    std::vector<Object> objects(controlled_count + 1);
    std::uint64_t logical_epoch = 1;
    for (unsigned index = 0; index < objects.size(); ++index) {
        auto& object = objects[index];
        check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&object.resource)), "Create object");
        object.resource_id = observer.observe_committed_resource(device.Get(), desc, object.resource.Get());
        if (!object.resource_id) throw std::runtime_error("observe resource failed");
        object.pattern = index + 17;
        object.controlled = index < controlled_count;
        if (object.controlled) {
            object.residency_id = 1000 + index;
            if (!runtime.register_controlled_resource({
                .id=object.residency_id,
                .resource=object.resource_id,
                .state=arc::ResidencyState::Resident,
                .safety=arc::ResidencySafety::ControlledSafe,
                .cost={.bytes=object_bytes,.reload_ms=.25}})) {
                throw std::runtime_error("runtime registration failed");
            }
        }

        void* mapped{};
        D3D12_RANGE empty{};
        check(upload->Map(0, &empty, &mapped), "Upload Map");
        std::memset(mapped, static_cast<int>(object.pattern), static_cast<std::size_t>(object_bytes));
        upload->Unmap(0, nullptr);
        reset();
        list->CopyResource(object.resource.Get(), upload.Get());
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = object.resource.Get();
        barrier.Transition.Subresource = UINT_MAX;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        execute();
        if (!runtime.note_use(object.resource_id, logical_epoch++, 1, fence_value, fence_value)) {
            throw std::runtime_error("initial runtime use failed");
        }
    }

    auto verify_use = [&](unsigned index, bool verify_all) {
        auto& object = objects[index];
        reset();
        list->CopyResource(readback.Get(), object.resource.Get());
        execute();
        if (!runtime.note_use(object.resource_id, logical_epoch++, 1, fence_value, fence_value)) {
            throw std::runtime_error("runtime use failed");
        }
        void* mapped{};
        D3D12_RANGE range{0, static_cast<SIZE_T>(object_bytes)};
        check(readback->Map(0, &range, &mapped), "Readback Map");
        const auto* bytes = static_cast<const unsigned char*>(mapped);
        bool ok = bytes[0] == object.pattern && bytes[object_bytes - 1] == object.pattern;
        if (verify_all) {
            for (std::uint64_t i = 0; ok && i < object_bytes; ++i) ok = bytes[i] == object.pattern;
        }
        D3D12_RANGE empty{};
        readback->Unmap(0, &empty);
        return ok;
    };

    // Train an observable unknown A -> controlled B sequence using actual GPU uses.
    const unsigned a_index = unknown_index;
    const unsigned b_index = 1;
    runtime.reset_sequence_context();
    for (unsigned i = 0; i < 6; ++i) {
        if (!verify_use(a_index, false) || !verify_use(b_index, false)) throw std::runtime_error("training verification failed");
    }

    const auto dxgi_before = arc::dx12::query_memory_budget(adapter.Get());
    if (!dxgi_before) throw std::runtime_error("DXGI budget unavailable");

    arc::MemoryBudgetPayload synthetic_pressure{};
    synthetic_pressure.local_budget = 64ull * 1024ull * 1024ull;
    synthetic_pressure.local_usage = 63ull * 1024ull * 1024ull;
    runtime.update_budget(synthetic_pressure);
    auto pressure_plan = runtime.plan(logical_epoch);
    if (pressure_plan.pressure != arc::PressureState::Emergency || pressure_plan.pressure_relief.arbitration.actions.empty()) {
        throw std::runtime_error("live pressure plan missing");
    }
    auto resolved = runtime.resolve_pressure_actions(pressure_plan.pressure_relief, logical_epoch);
    if (!resolved || resolved->empty()) throw std::runtime_error("pressure resolve failed");

    std::uint64_t planned_evicted{};
    unsigned eviction_actions{};
    bool unknown_mutated{};
    for (const auto& value : *resolved) {
        const auto* action = std::get_if<arc::ResidencyAction>(&value);
        if (!action) throw std::runtime_error("unexpected texture action in buffer lab");
        const auto object = runtime.residency().find(action->object);
        if (!object) throw std::runtime_error("resolved residency missing");
        auto it = std::find_if(objects.begin(), objects.end(), [&](const Object& entry){ return entry.resource_id == object->resource; });
        if (it == objects.end() || !it->controlled) { unknown_mutated = true; throw std::runtime_error("uncontrolled resource selected"); }
        if (backend.evict_after(it->resource.Get(), fence.Get(), action->required_fence) != arc::dx12::ResidencyResult::Success) {
            throw std::runtime_error("physical Evict failed");
        }
        if (!runtime.begin_residency_action(*action, logical_epoch)) throw std::runtime_error("runtime eviction bookkeeping failed");
        planned_evicted += action->bytes;
        ++eviction_actions;
    }

    std::uint64_t observed_relief{};
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        const auto after = arc::dx12::query_memory_budget(adapter.Get());
        if (after && dxgi_before->local_usage > after->local_usage) {
            observed_relief = (std::max)(observed_relief, dxgi_before->local_usage - after->local_usage);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Ensure B is evicted so the learned A->B transition can exercise prefetch.
    auto b_state = runtime.residency().find(objects[b_index].residency_id);
    if (!b_state) throw std::runtime_error("B state missing");
    if (b_state->state == arc::ResidencyState::Resident) {
        arc::ResidencyAction evict_b{
            arc::ResidencyAction::Type::Evict,
            objects[b_index].residency_id,
            object_bytes,
            0,
            1.0,
            b_state->last_use_queue,
            b_state->last_use_fence};
        if (backend.evict_after(objects[b_index].resource.Get(), fence.Get(), evict_b.required_fence) != arc::dx12::ResidencyResult::Success) {
            throw std::runtime_error("B Evict failed");
        }
        if (!runtime.begin_residency_action(evict_b, logical_epoch)) throw std::runtime_error("B bookkeeping failed");
    }

    arc::MemoryBudgetPayload normal{};
    normal.local_budget = 64ull * 1024ull * 1024ull;
    normal.local_usage = 16ull * 1024ull * 1024ull;
    runtime.update_budget(normal); // Emergency -> Pressure
    runtime.update_budget(normal); // Pressure -> Normal
    if (runtime.residency().pressure() != arc::PressureState::Normal) throw std::runtime_error("pressure recovery failed");

    runtime.reset_sequence_context();
    if (!verify_use(a_index, false)) throw std::runtime_error("A verification failed");
    const auto prefetch_plan = runtime.plan(logical_epoch);
    auto prefetch = std::find_if(prefetch_plan.transition_prefetch.begin(), prefetch_plan.transition_prefetch.end(), [&](const auto& action) {
        return action.object == objects[b_index].residency_id;
    });
    if (prefetch == prefetch_plan.transition_prefetch.end()) throw std::runtime_error("transition prefetch did not predict B");

    const auto ticket = backend.enqueue_make_resident(objects[b_index].resource.Get());
    if (ticket.result != arc::dx12::ResidencyResult::Success) throw std::runtime_error("EnqueueMakeResident failed");
    if (!runtime.begin_residency_action(*prefetch, logical_epoch, false)) throw std::runtime_error("prefetch bookkeeping failed");
    check(queue->Wait(ticket.fence, ticket.value), "Queue residency Wait");
    if (!verify_use(b_index, true)) throw std::runtime_error("prefetched B contents corrupted");
    if (!runtime.complete_make_resident(objects[b_index].residency_id)) throw std::runtime_error("complete MakeResident failed");

    // Restore remaining evicted controlled resources before final integrity pass.
    for (unsigned index = 0; index < controlled_count; ++index) {
        auto state = runtime.residency().find(objects[index].residency_id);
        if (!state) throw std::runtime_error("final state missing");
        if (state->state == arc::ResidencyState::Evicted) {
            const auto action = runtime.residency().require_resident(objects[index].residency_id);
            if (!action || backend.make_resident_and_wait(objects[index].resource.Get()) != arc::dx12::ResidencyResult::Success) {
                throw std::runtime_error("final MakeResident failed");
            }
            if (!runtime.begin_residency_action(*action, logical_epoch, false) ||
                !runtime.complete_make_resident(objects[index].residency_id)) {
                throw std::runtime_error("final runtime restore failed");
            }
        }
    }

    bool contents_verified = true;
    for (unsigned index = 0; index < objects.size(); ++index) contents_verified &= verify_use(index, true);
    const bool unknown_untouched = !runtime.controlled(objects[unknown_index].resource_id) && !unknown_mutated;
    const auto metrics = runtime.metrics();
    const bool transition_prefetch_verified = metrics.transition_prefetch_actions > 0;
    const bool valid = contents_verified && unknown_untouched && transition_prefetch_verified &&
        eviction_actions > 0 && debug_errors.load(std::memory_order_relaxed) == 0;

    std::filesystem::create_directories("traces");
    std::ofstream out("traces/live-runtime-lab.json", std::ios::trunc);
    out << "{\n"
        << "  \"schema\": 1,\n"
        << "  \"valid\": " << (valid ? "true" : "false") << ",\n"
        << "  \"controlled_resources\": " << controlled_count << ",\n"
        << "  \"uncontrolled_resources\": 1,\n"
        << "  \"eviction_actions\": " << eviction_actions << ",\n"
        << "  \"planned_evicted_bytes\": " << planned_evicted << ",\n"
        << "  \"dxgi_observed_relief_bytes\": " << observed_relief << ",\n"
        << "  \"transition_prefetch_actions\": " << metrics.transition_prefetch_actions << ",\n"
        << "  \"transition_prefetch_verified\": " << (transition_prefetch_verified ? "true" : "false") << ",\n"
        << "  \"unknown_resource_untouched\": " << (unknown_untouched ? "true" : "false") << ",\n"
        << "  \"contents_verified\": " << (contents_verified ? "true" : "false") << ",\n"
        << "  \"resolve_failures\": " << metrics.plan_resolve_failures << ",\n"
        << "  \"debug_error_count\": " << debug_errors.load(std::memory_order_relaxed) << "\n"
        << "}\n";

    std::cout << "live-runtime valid=" << valid
              << " evictions=" << eviction_actions
              << " relief=" << observed_relief
              << " prefetch=" << metrics.transition_prefetch_actions
              << " debug_errors=" << debug_errors.load(std::memory_order_relaxed) << '\n';
    return valid ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "live-runtime-lab: " << error.what() << '\n';
    return 1;
}
