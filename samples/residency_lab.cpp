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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

void check(HRESULT hr, const char* what = "D3D12 call") {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(what) + " failed: " + std::to_string(static_cast<unsigned>(hr)));
    }
}

D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

enum class LabMode { Baseline, Oracle, Autonomous };
enum class WorkloadPattern { Mixed, Cycle, Phase, Streaming };

LabMode parse_mode(const std::string& value) {
    if (value == "baseline") return LabMode::Baseline;
    if (value == "arc") return LabMode::Oracle;
    if (value == "auto") return LabMode::Autonomous;
    throw std::runtime_error("mode must be baseline|arc|auto");
}

WorkloadPattern parse_pattern(const std::string& value) {
    if (value == "mixed") return WorkloadPattern::Mixed;
    if (value == "cycle") return WorkloadPattern::Cycle;
    if (value == "phase") return WorkloadPattern::Phase;
    if (value == "stream") return WorkloadPattern::Streaming;
    throw std::runtime_error("pattern must be mixed|cycle|phase|stream");
}

const char* mode_name(LabMode mode) {
    switch (mode) {
        case LabMode::Baseline: return "baseline";
        case LabMode::Oracle: return "arc";
        case LabMode::Autonomous: return "auto";
    }
    return "unknown";
}

const char* pattern_name(WorkloadPattern pattern) {
    switch (pattern) {
        case WorkloadPattern::Mixed: return "mixed";
        case WorkloadPattern::Cycle: return "cycle";
        case WorkloadPattern::Phase: return "phase";
        case WorkloadPattern::Streaming: return "stream";
    }
    return "unknown";
}

unsigned workload_index(WorkloadPattern pattern, std::size_t epoch, unsigned count) {
    const unsigned safe_count = count - 2;
    switch (pattern) {
        case WorkloadPattern::Mixed:
            if (epoch % 200 == 150 && safe_count > 6) {
                return 4 + static_cast<unsigned>((epoch / 200) % (safe_count - 4));
            }
            if (epoch % 50 == 25 && safe_count > 5) return 5;
            if (epoch % 20 == 10 && safe_count > 4) return 4;
            return static_cast<unsigned>(epoch % (std::min)(4U, safe_count));
        case WorkloadPattern::Cycle:
            return static_cast<unsigned>(epoch % (std::min)(6U, safe_count));
        case WorkloadPattern::Phase: {
            const unsigned width = (std::min)(4U, (std::max)(2U, safe_count / 2));
            const unsigned second_base = safe_count > width ? safe_count - width : 0;
            const bool second = (epoch / 900) % 2 != 0;
            return (second ? second_base : 0U) + static_cast<unsigned>(epoch % width);
        }
        case WorkloadPattern::Streaming:
            return static_cast<unsigned>((epoch / 60) % safe_count);
    }
    return 0;
}

struct Object {
    arc::ResourceId resource{};
    arc::ResidencyId residency{};
    ComPtr<ID3D12Resource> object;
    bool evicted{};
    bool pending{};
    std::uint64_t evicted_epoch{};
    std::uint64_t residency_fence{};
    ID3D12Fence* residency_fence_object{};
};

}  // namespace

int main(int argc, char** argv) try {
    const auto mode = parse_mode(argc > 1 ? argv[1] : "arc");
    const bool controlled = mode != LabMode::Baseline;
    const bool autonomous = mode == LabMode::Autonomous;
    const unsigned count = argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 24;
    const UINT64 bytes = (argc > 3 ? std::stoull(argv[3]) : 8ULL) * 1024ULL * 1024ULL;
    const unsigned managed_percent = argc > 4 ? static_cast<unsigned>(std::stoul(argv[4])) : 50;
    const auto pattern = parse_pattern(argc > 5 ? argv[5] : "mixed");
    const std::size_t warmup_epochs = argc > 6 ? std::stoull(argv[6]) : 400;
    const std::size_t measured_epochs = argc > 7 ? std::stoull(argv[7]) : 2000;

    if (count < 8 || count > 128 || bytes < 65536 || bytes > 64ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error("invalid count/size");
    }
    if (managed_percent < 35 || managed_percent > 100) {
        throw std::runtime_error("managed_percent must be 35..100");
    }
    if (warmup_epochs < 50 || measured_epochs < 100 || measured_epochs > 20000) {
        throw std::runtime_error("invalid warmup/measured epoch count");
    }

    const bool debug_enabled = std::getenv("ARC_D3D12_DEBUG") != nullptr;
    if (debug_enabled) {
        ComPtr<ID3D12Debug> debug;
        check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12GetDebugInterface");
        debug->EnableDebugLayer();
    }

    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    ComPtr<ID3D12InfoQueue> info;
    if (debug_enabled) check(device.As(&info), "ID3D12InfoQueue");

    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter3> adapter;
    check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid");

    arc::dx12::BudgetNotification notification(adapter.Get());
    if (!notification.valid()) throw std::runtime_error("budget notification unavailable");
    arc::dx12::ResidencyBackend residency_backend(device.Get());

    arc::ResidencyPolicyConfig governor_config{};
    if (autonomous) {
        governor_config.minimum_residency_age_epochs = 4;
        governor_config.prefetch_horizon_epochs = 8;
        governor_config.eviction_prediction_guard_epochs = 32;
        governor_config.post_miss_grace_epochs = 24;
        governor_config.emergency_target = .72;
        governor_config.pressure_target = .78;
        governor_config.promotion_ceiling = .84;
        governor_config.recovery_samples = 2;
    }
    arc::ResidencyGovernor governor(governor_config);

    std::filesystem::create_directories("traces");
    const std::string stem = std::string("residency-lab-") + mode_name(mode);
    arc::Session session("traces/" + stem + ".arcbin", 65536);
    arc::IdAllocator ids;
    arc::dx12::Observer observer(session.ring(), ids);
    auto emit = [&](arc::EventType type, const auto& payload) {
        if (!observer.observe(type, payload)) throw std::runtime_error("trace overflow");
    };
    auto query_budget = [&] {
        const auto value = arc::dx12::query_memory_budget(adapter.Get());
        if (!value) throw std::runtime_error("budget unavailable");
        emit(arc::EventType::MemoryBudgetSample, *value);
        return *value;
    };
    const auto before = query_budget();

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto description = buffer(bytes);

    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> readback;
    check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "Create upload");
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create readback");

    std::vector<Object> objects;
    objects.reserve(count);
    for (unsigned index = 0; index < count; ++index) {
        Object object;
        check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&object.object)), "Create object");
        object.resource = observer.observe_committed_resource(device.Get(), description, object.object.Get());
        object.residency = ids.next();
        const auto safety = index + 2 < count ? arc::ResidencySafety::ControlledSafe
            : index + 1 == count ? arc::ResidencySafety::Unknown : arc::ResidencySafety::Pinned;
        if (!governor.register_object({.id=object.residency,.resource=object.resource,.state=arc::ResidencyState::Resident,.safety=safety,.cost={.bytes=bytes,.reload_ms=.25}})) {
            throw std::runtime_error("registration failed");
        }
        objects.push_back(std::move(object));
    }
    const auto allocated = query_budget();

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList> commands;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "CreateCommandList");
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready) throw std::runtime_error("event creation failed");

    const auto queue_id = ids.next();
    const auto command_id = ids.next();
    std::uint64_t fence_value{};
    emit(arc::EventType::CommandQueueCreated, arc::QueueCreatePayload{.queue=queue_id,.type=arc::QueueClass::Graphics});
    emit(arc::EventType::CommandListCreated, arc::CommandListPayload{.command=command_id});

    auto execute = [&] {
        check(commands->Close(), "CommandList Close");
        emit(arc::EventType::CommandListClosed, arc::CommandListPayload{.command=command_id});
        ID3D12CommandList* lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        emit(arc::EventType::QueueSubmit, arc::QueueSubmitPayload{.queue=queue_id,.command=command_id,.submission=fence_value + 1});
        check(queue->Signal(fence.Get(), ++fence_value), "Queue Signal");
        check(fence->SetEventOnCompletion(fence_value, ready), "SetEventOnCompletion");
        if (WaitForSingleObject(ready, 30000) != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
    };
    auto reset = [&] {
        check(allocator->Reset(), "Allocator Reset");
        check(commands->Reset(allocator.Get(), nullptr), "CommandList Reset");
        emit(arc::EventType::CommandListReset, arc::CommandListPayload{.command=command_id});
    };

    // Initialize every object with a deterministic byte pattern and leave it in COPY_SOURCE state.
    for (unsigned index = 0; index < count; ++index) {
        void* mapped{};
        D3D12_RANGE empty{};
        check(upload->Map(0, &empty, &mapped), "Upload Map");
        std::memset(mapped, static_cast<int>(index + 1), static_cast<std::size_t>(bytes));
        upload->Unmap(0, nullptr);
        if (index) reset();
        commands->CopyResource(objects[index].object.Get(), upload.Get());
        emit(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command=command_id,.resource=objects[index].resource,.write=1});
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = objects[index].object.Get();
        barrier.Transition.Subresource = UINT_MAX;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        emit(arc::EventType::Barrier, arc::BarrierPayload{.resource=objects[index].resource,.command=command_id,.before_state=D3D12_RESOURCE_STATE_COPY_DEST,.after_state=D3D12_RESOURCE_STATE_COPY_SOURCE,.subresource=UINT_MAX});
        execute();
        governor.note_use(objects[index].residency, index + 1, queue_id, fence_value, fence_value);
    }

    if (residency_backend.evict_after(objects[0].object.Get(), fence.Get(), fence_value + 1) != arc::dx12::ResidencyResult::UnsafeInFlight) {
        throw std::runtime_error("unsafe eviction was not rejected");
    }

    std::uint64_t notification_count{};
    if (notification.wait(100)) ++notification_count;

    const unsigned target_objects_raw = static_cast<unsigned>((static_cast<std::uint64_t>(count) * managed_percent + 99) / 100);
    const unsigned managed_target_objects = (std::min)(count, (std::max)(4U, target_objects_raw));
    const std::uint64_t managed_budget = static_cast<std::uint64_t>(managed_target_objects) * bytes;
    std::uint64_t managed_resident_bytes = static_cast<std::uint64_t>(count) * bytes;
    std::uint64_t managed_peak_measured{};
    std::uint64_t managed_sum{};
    std::uint64_t bytes_evicted{}, bytes_resident{}, reloads{}, useful{}, false_evictions{}, late{}, speculative_promotions{};
    constexpr std::size_t oracle_lookahead = 8;

    auto find_index = [&](arc::ResidencyId residency) -> unsigned {
        const auto found = std::find_if(objects.begin(), objects.end(), [&](const Object& value){ return value.residency == residency; });
        if (found == objects.end()) throw std::runtime_error("planned object missing");
        return static_cast<unsigned>(std::distance(objects.begin(), found));
    };

    auto evict = [&](unsigned index, std::uint64_t required_fence, std::uint64_t epoch) {
        auto& object = objects[index];
        if (object.evicted || object.pending) return;
        if (residency_backend.evict_after(object.object.Get(), fence.Get(), required_fence) != arc::dx12::ResidencyResult::Success) {
            throw std::runtime_error("Evict failed");
        }
        if (!governor.transition(object.residency, arc::ResidencyState::Resident, arc::ResidencyState::Evicted)) {
            throw std::runtime_error("invalid governor eviction");
        }
        object.evicted = true;
        object.evicted_epoch = epoch;
        bytes_evicted += bytes;
        ++useful;
        if (autonomous) {
            governor.record_eviction(object.residency, false, epoch);
            if (managed_resident_bytes < bytes) throw std::runtime_error("managed residency underflow");
            managed_resident_bytes -= bytes;
        }
        emit(arc::EventType::ResidencyTransition, arc::ResidencyTransitionPayload{.object=object.residency,.before=arc::ResidencyState::Resident,.after=arc::ResidencyState::Evicted,.fence_value=required_fence,.bytes=bytes});
    };

    auto make_resident = [&](unsigned index, std::uint64_t epoch, bool speculative) {
        auto& object = objects[index];
        if (!object.evicted) return;
        if (!governor.require_resident(object.residency)) throw std::runtime_error("resident demand rejected");
        if (!governor.transition(object.residency, arc::ResidencyState::Evicted, arc::ResidencyState::PendingResident)) throw std::runtime_error("pending transition failed");
        const auto ticket = residency_backend.enqueue_make_resident(object.object.Get());
        if (ticket.result != arc::dx12::ResidencyResult::Success) throw std::runtime_error("asynchronous MakeResident failed");
        false_evictions += epoch <= object.evicted_epoch + 4;
        object.evicted = false;
        object.pending = true;
        object.residency_fence = ticket.value;
        object.residency_fence_object = ticket.fence;
        bytes_resident += bytes;
        ++reloads;
        if (speculative) ++speculative_promotions;
        if (autonomous) {
            governor.record_resident(object.residency, !speculative, epoch);
            managed_resident_bytes += bytes;
        }
        emit(arc::EventType::ResidencyTransition, arc::ResidencyTransitionPayload{.object=object.residency,.before=arc::ResidencyState::Evicted,.after=arc::ResidencyState::PendingResident,.fence_value=ticket.value,.bytes=bytes});
    };

    bool contents = true;
    auto use_object = [&](unsigned index, std::uint64_t logical_epoch, bool measure, std::vector<double>& times) {
        const auto start = std::chrono::steady_clock::now();
        if (objects[index].evicted) {
            ++late;
            make_resident(index, logical_epoch, false);
        }
        if (objects[index].pending) {
            check(queue->Wait(objects[index].residency_fence_object, objects[index].residency_fence), "Queue residency Wait");
        } else if (controlled && index + 2 < count) {
            const auto state = governor.find(objects[index].residency);
            if (!state || state->state != arc::ResidencyState::Resident) throw std::runtime_error("object is not resident before use");
        }

        reset();
        commands->CopyResource(readback.Get(), objects[index].object.Get());
        emit(arc::EventType::ResourceUse, arc::ResourceUsePayload{.command=command_id,.resource=objects[index].resource});
        execute();
        if (objects[index].pending) {
            if (!governor.transition(objects[index].residency, arc::ResidencyState::PendingResident, arc::ResidencyState::Resident)) throw std::runtime_error("resident transition failed");
            objects[index].pending = false;
            emit(arc::EventType::ResidencyTransition, arc::ResidencyTransitionPayload{.object=objects[index].residency,.before=arc::ResidencyState::PendingResident,.after=arc::ResidencyState::Resident,.fence_value=fence_value,.bytes=bytes});
        }

        void* mapped{};
        D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
        check(readback->Map(0, &range, &mapped), "Readback Map");
        const auto expected = static_cast<unsigned char>(index + 1);
        const auto* data = static_cast<const unsigned char*>(mapped);
        for (UINT64 offset = 0; offset < bytes; ++offset) {
            if (data[offset] != expected) { contents = false; break; }
        }
        D3D12_RANGE empty{};
        readback->Unmap(0, &empty);
        governor.note_use(objects[index].residency, logical_epoch, queue_id, fence_value, fence->GetCompletedValue());
        if (measure) times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    };

    std::vector<double> times;
    // Equal warmup for all modes removes a baseline-vs-auto thermal/history bias.
    for (std::size_t epoch = 0; epoch < warmup_epochs; ++epoch) {
        use_object(workload_index(pattern, epoch, count), count + epoch + 1, false, times);
    }

    if (mode == LabMode::Oracle) {
        for (unsigned index = 0; index + 2 < count; ++index) {
            bool needed_soon = false;
            for (std::size_t future = 0; future < oracle_lookahead; ++future) {
                needed_soon |= workload_index(pattern, warmup_epochs + future, count) == index;
            }
            if (!needed_soon) evict(index, governor.find(objects[index].residency)->last_use_fence, warmup_epochs);
        }
        for (std::size_t future = 0; future < oracle_lookahead; ++future) {
            make_resident(workload_index(pattern, warmup_epochs + future, count), warmup_epochs, true);
        }
    }

    const auto after_initial_policy = query_budget();
    for (std::size_t epoch = 0; epoch < measured_epochs; ++epoch) {
        const auto logical_epoch = count + warmup_epochs + epoch + 1;
        if (autonomous) {
            governor.update_budget(managed_budget, managed_resident_bytes);
            for (const auto& action : governor.plan_evictions(logical_epoch)) {
                if (action.required_queue && action.required_queue != queue_id) throw std::runtime_error("unexpected eviction queue");
                evict(find_index(action.object), action.required_fence, logical_epoch);
            }
            governor.update_budget(managed_budget, managed_resident_bytes);
            for (const auto& action : governor.plan_promotions(logical_epoch)) {
                make_resident(find_index(action.object), logical_epoch, true);
            }
        }

        const auto index = workload_index(pattern, warmup_epochs + epoch, count);
        use_object(index, logical_epoch, true, times);

        if (mode == LabMode::Oracle) {
            bool needed_soon = false;
            for (std::size_t future = epoch + 1; future < (std::min)(measured_epochs, epoch + oracle_lookahead + 1); ++future) {
                needed_soon |= workload_index(pattern, warmup_epochs + future, count) == index;
            }
            if (index + 2 < count && !needed_soon) evict(index, governor.find(objects[index].residency)->last_use_fence, logical_epoch);
            if (epoch + oracle_lookahead < measured_epochs) {
                make_resident(workload_index(pattern, warmup_epochs + epoch + oracle_lookahead, count), logical_epoch, true);
            }
        }

        if (autonomous) {
            managed_peak_measured = (std::max)(managed_peak_measured, managed_resident_bytes);
            managed_sum += managed_resident_bytes;
        }
    }

    for (auto& object : objects) {
        if (object.evicted) {
            if (residency_backend.make_resident_and_wait(object.object.Get()) != arc::dx12::ResidencyResult::Success) throw std::runtime_error("final MakeResident failed");
            if (governor.transition(object.residency, arc::ResidencyState::Evicted, arc::ResidencyState::PendingResident)) {
                (void)governor.transition(object.residency, arc::ResidencyState::PendingResident, arc::ResidencyState::Resident);
            }
            object.evicted = false;
            bytes_resident += bytes;
            ++reloads;
        }
        object.object.Reset();
        observer.observe_resource_destroyed(object.resource);
    }

    CloseHandle(ready);
    session.finish();
    const auto final_budget = arc::dx12::query_memory_budget(adapter.Get());

    bool debug_clean = true;
    if (info) {
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T size{};
            info->GetMessage(i, nullptr, &size);
            std::vector<std::byte> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            info->GetMessage(i, message, &size);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << message->pDescription << '\n';
                debug_clean = false;
            }
        }
    }

    const auto stats = arc::summarize(times);
    const bool valid = contents && debug_clean && session.complete() && session.graph().errors() == 0 && (mode != LabMode::Oracle || late == 0);
    const auto average_managed = autonomous && measured_epochs ? managed_sum / measured_epochs : 0;
    const auto policy_metrics = governor.metrics();

    std::ofstream report("traces/" + stem + ".json");
    report << "{\"schema\":3,\"mode\":\"" << mode_name(mode) << "\",\"pattern\":\"" << pattern_name(pattern)
        << "\",\"valid\":" << (valid ? "true" : "false") << ",\"objects\":" << count << ",\"object_bytes\":" << bytes
        << ",\"managed_percent\":" << managed_percent << ",\"warmup_epochs\":" << warmup_epochs << ",\"measured_epochs\":" << measured_epochs
        << ",\"managed_budget\":" << (autonomous ? managed_budget : 0) << ",\"managed_usage_final\":" << (autonomous ? managed_resident_bytes : 0)
        << ",\"managed_usage_average\":" << average_managed << ",\"managed_usage_peak\":" << (autonomous ? managed_peak_measured : 0)
        << ",\"contents_identical\":" << (contents ? "true" : "false") << ",\"bytes_evicted\":" << bytes_evicted << ",\"bytes_made_resident\":" << bytes_resident
        << ",\"useful_evictions\":" << useful << ",\"false_evictions\":" << false_evictions << ",\"reloads\":" << reloads << ",\"late_residency\":" << late
        << ",\"compulsory_misses\":" << (autonomous ? policy_metrics.compulsory_misses : 0)
        << ",\"predictable_misses\":" << (autonomous ? policy_metrics.predictable_misses : 0)
        << ",\"speculative_promotions\":" << speculative_promotions << ",\"budget_notifications\":" << notification_count
        << ",\"budget\":" << before.local_budget << ",\"usage_before\":" << before.local_usage << ",\"usage_allocated\":" << allocated.local_usage
        << ",\"usage_after_initial_policy\":" << after_initial_policy.local_usage << ",\"usage_final\":" << (final_budget ? final_budget->local_usage : 0)
        << ",\"p50_ms\":" << stats.median << ",\"p95_ms\":" << stats.p95 << ",\"p99_ms\":" << stats.p99
        << ",\"trace_drops\":" << session.dropped() << ",\"graph_errors\":" << session.graph().errors() << "}\n";

    std::cout << stem << " pattern=" << pattern_name(pattern) << " managed=" << managed_percent << "% valid=" << valid
              << " evicted_MiB=" << bytes_evicted / 1024 / 1024 << " reloads=" << reloads << " late=" << late
              << " compulsory=" << (autonomous ? policy_metrics.compulsory_misses : 0)
              << " predictable=" << (autonomous ? policy_metrics.predictable_misses : 0)
              << " p99_ms=" << stats.p99 << '\n';
    return valid ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
