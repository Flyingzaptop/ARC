#include "stdafx.h"
#include "ArcWickedBridge.h"
#include "ArcWickedHooks.h"
#include "ArcWickedTelemetry.h"
#include "ArcWickedSemanticAudit.h"

#include "arc/dx12_host_adapter.hpp"
#include "arc/quality_profile.hpp"
#include "arc/resource_semantics.hpp"
#include "arc/scene_understanding.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <ostream>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
constexpr char kWickedUpstreamSha[] = "0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7";
constexpr std::uint64_t kShadowProfile = 145001;
constexpr std::uint64_t kGeometryProfile = 145002;
constexpr std::uint64_t kBandwidthProfile = 145003;

struct Stats
{
    std::vector<double> frames;
    std::uint64_t misses = 0;
    double overshoot = 0.0;
};

struct CommandCounters
{
    std::uint64_t draws = 0;
    std::uint64_t indexed_draws = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t indirect = 0;
    std::uint64_t draw_items = 0;
    std::uint64_t dispatch_groups = 0;
};

struct CommandPending
{
    std::unordered_map<ID3D12Resource*, arc_wicked::AccessMask> uses;
    CommandCounters counters;
};

struct QueueState
{
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::uint64_t value = 0;
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
};

struct SceneSpec
{
    int combo_index;
    const char* name;
};

struct SceneSemanticCapture
{
    bool captured = false;
    arc::SceneSemanticSignature signature{};
    arc::SceneClusterAssignment cluster{};
};

constexpr std::array<SceneSpec, 5> kScenes{{
    {1, "model"},
    {7, "shadows"},
    {6, "water"},
    {12, "volumetric"},
    {18, "instances_65k"},
}};

enum class Phase
{
    Dormant,
    Warmup,
    Baseline,
    Adaptive,
    Recovery,
    Done,
};

// Timed scopes include host/bridge mutex waits and work on recording threads.
// Sum is thread-time, not critical-path frame time; recording cost is excluded.
std::array<arc_wicked::HookTiming, 64> g_hook_timings{};
std::atomic<bool> g_hooks_enabled{true};
std::atomic<bool> g_hook_timing_enabled{true};
std::atomic<std::uint64_t> g_present_failures{0};

void WriteHookTimings(std::ostream& out)
{
    out << "{\"enabled\":" << (g_hook_timing_enabled ? "true" : "false")
        << ",\"scope\":\"inclusive_thread_time_mutex_wait_included\",\"hooks\":{";
    bool first = true;
    for (std::size_t i = 0; i < g_hook_timings.size(); ++i)
    {
        if (!g_hook_timings[i].calls.load(std::memory_order_relaxed)) continue;
        if (!first) out << ',';
        first = false;
        out << '\"' << i << "\":";
        g_hook_timings[i].write_json(out);
    }
    out << "}}";
}

// Optional timing lets comparative runs avoid instrumentation perturbation.
class OptionalHookTimer {
public:
    explicit OptionalHookTimer(unsigned hook) noexcept
        : hook_(hook), enabled_(g_hook_timing_enabled.load(std::memory_order_relaxed)) {
        if (enabled_) start_ = std::chrono::steady_clock::now();
    }
    ~OptionalHookTimer() {
        if (enabled_) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start_).count();
            g_hook_timings[hook_].record(ns > 0 ? static_cast<std::uint64_t>(ns) : 0);
        }
    }
private:
    unsigned hook_;
    bool enabled_;
    std::chrono::steady_clock::time_point start_{};
};

std::atomic<std::uint32_t> g_arc_last_hook{0};
std::atomic<std::uint32_t> g_arc_last_scene{0};
std::atomic<std::uint32_t> g_arc_last_phase{0};

int EnvInt(const wchar_t* name, int fallback, int minimum, int maximum)
{
    wchar_t buffer[64]{};
    const DWORD capacity = static_cast<DWORD>(_countof(buffer));
    const DWORD count = GetEnvironmentVariableW(name, buffer, capacity);
    if (count == 0 || count >= capacity) return fallback;
    return std::clamp(_wtoi(buffer), minimum, maximum);
}

std::wstring EnvString(const wchar_t* name)
{
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (!written) return {};
    value.resize(written);
    return value;
}

void WriteDeviceLossContext(HRESULT present_result, HRESULT device_reason) noexcept
{
    const auto path = EnvString(L"ARC_WICKED_CRASH_OUTPUT");
    if (path.empty()) return;
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    char buffer[512]{};
    const int n = sprintf_s(
        buffer, sizeof(buffer),
        "device_loss=1\r\npresent_hresult=0x%08lX\r\ndevice_removed_reason=0x%08lX\r\nlast_hook=%u\r\nscene_index=%u\r\nphase=%u\r\n",
        static_cast<unsigned long>(present_result),
        static_cast<unsigned long>(device_reason),
        g_arc_last_hook.load(std::memory_order_relaxed),
        g_arc_last_scene.load(std::memory_order_relaxed),
        g_arc_last_phase.load(std::memory_order_relaxed));
    if (n > 0)
    {
        DWORD written = 0;
        WriteFile(file, buffer, static_cast<DWORD>(n), &written, nullptr);
    }
    CloseHandle(file);
}

std::string Narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}

double Percentile(std::vector<double> values, double p)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = static_cast<std::size_t>(std::ceil(pos));
    if (lo == hi) return values[lo];
    const double t = pos - static_cast<double>(lo);
    return values[lo] * (1.0 - t) + values[hi] * t;
}

double MissRatio(const Stats& s)
{
    return s.frames.empty() ? 0.0 : static_cast<double>(s.misses) / static_cast<double>(s.frames.size());
}

double MeanOvershoot(const Stats& s)
{
    return s.misses ? s.overshoot / static_cast<double>(s.misses) : 0.0;
}

template<std::size_t N>
void WriteFloatArray(std::ostream& out, const std::array<float, N>& values)
{
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        if (i) out << ",";
        out << values[i];
    }
    out << "]";
}

void WriteSceneSemanticCapture(std::ostream& out, const SceneSemanticCapture& capture)
{
    const auto& signature = capture.signature;
    out << "{\"captured\":" << (capture.captured ? "true" : "false")
        << ",\"history_complete\":" << (signature.history_complete ? "true" : "false")
        << ",\"cluster\":" << capture.cluster.cluster
        << ",\"cluster_distance\":" << capture.cluster.distance
        << ",\"cluster_confidence\":" << capture.cluster.confidence
        << ",\"cluster_created\":" << (capture.cluster.created ? "true" : "false")
        << ",\"frame\":" << signature.frame
        << ",\"window_frames\":" << signature.window_frames
        << ",\"active_resources\":" << signature.active_resources
        << ",\"known_resources\":" << signature.known_resources
        << ",\"active_bytes\":" << signature.active_bytes
        << ",\"known_bytes\":" << signature.known_bytes
        << ",\"coverage\":" << signature.coverage
        << ",\"mean_confidence\":" << signature.mean_confidence
        << ",\"read_fraction\":" << signature.read_fraction
        << ",\"write_fraction\":" << signature.write_fraction
        << ",\"multi_queue_fraction\":" << signature.multi_queue_fraction
        << ",\"submissions\":" << signature.submissions
        << ",\"draws\":" << signature.draws
        << ",\"indexed_draws\":" << signature.indexed_draws
        << ",\"dispatches\":" << signature.dispatches
        << ",\"indirect\":" << signature.indirect
        << ",\"draw_items\":" << signature.draw_items
        << ",\"dispatch_groups\":" << signature.dispatch_groups
        << ",\"copies\":" << signature.copies
        << ",\"resource_accesses_per_frame\":" << signature.resource_accesses_per_frame
        << ",\"draw_calls_per_frame\":" << signature.draw_calls_per_frame
        << ",\"draw_items_per_frame\":" << signature.draw_items_per_frame
        << ",\"dispatches_per_frame\":" << signature.dispatches_per_frame
        << ",\"dispatch_groups_per_frame\":" << signature.dispatch_groups_per_frame
        << ",\"indirect_per_frame\":" << signature.indirect_per_frame
        << ",\"submissions_per_frame\":" << signature.submissions_per_frame
        << ",\"copies_per_frame\":" << signature.copies_per_frame
        << ",\"resource_fractions\":";
    WriteFloatArray(out, signature.resource_fractions);
    out << ",\"byte_fractions\":";
    WriteFloatArray(out, signature.byte_fractions);
    out << "}";
}

void Recompute(Stats& s, double target)
{
    s.misses = 0;
    s.overshoot = 0.0;
    for (const double x : s.frames)
    {
        if (x > target)
        {
            ++s.misses;
            s.overshoot += x - target;
        }
    }
}

std::uint64_t Delta(std::uint64_t now, std::uint64_t before)
{
    return now >= before ? now - before : 0;
}

class Bridge final
{
public:
    Bridge(
        ID3D12Device* device,
        ID3D12DescriptorHeap* resource_heap,
        ID3D12DescriptorHeap* sampler_heap)
        : device_(device)
    {
        seconds_ = EnvInt(L"ARC_WICKED_SECONDS", 30, 10, 120);
        warmup_seconds_ = EnvInt(L"ARC_WICKED_WARMUP_SECONDS", 5, 2, 20);
        recovery_seconds_ = EnvInt(L"ARC_WICKED_RECOVERY_SECONDS", 20, 5, 60);
        settle_milliseconds_ = EnvInt(L"ARC_WICKED_SCENE_SETTLE_MS", 1500, 500, 5000);
        control_frames_ = EnvInt(L"ARC_WICKED_CONTROL_FRAMES", 16, 4, 120);
        arc_source_sha_ = Narrow(EnvString(L"ARC_SOURCE_SHA"));
        const auto output = EnvString(L"ARC_WICKED_OUTPUT");
        if (!output.empty()) output_path_ = output;
        experiment_mode_ = Narrow(EnvString(L"ARC_WICKED_EXPERIMENT_MODE"));
        const auto cpu_output = EnvString(L"ARC_WICKED_CPU_PROFILE");
        if (!cpu_output.empty()) {
            cpu_profile_.open(std::filesystem::path(cpu_output), std::ios::trunc);
            if (!cpu_profile_) { phase_ = Phase::Done; PostQuitMessage(2); return; }
            cpu_profile_ << "elapsed_ms,frame,scene,event,ms\n";
            cpu_profile_scene_ = EnvInt(L"ARC_WICKED_CPU_SCENE", 1, 0, 18);
            cpu_profile_seconds_ = EnvInt(L"ARC_WICKED_CPU_SECONDS", 8, 2, 15);
            cpu_profile_start_ = std::chrono::steady_clock::now();
        }
        if (!experiment_mode_.empty() && experiment_mode_ != "off" &&
            experiment_mode_ != "observe" && experiment_mode_ != "adaptive")
        {
            phase_ = Phase::Done;
            PostQuitMessage(2);
            return;
        }
        scene_offset_ = static_cast<std::size_t>(EnvInt(L"ARC_WICKED_SCENE_OFFSET", 0, 0, 4));
        if (experiment_mode_ == "off") return;
        arc::dx12::NativeHostAdapterConfig cfg{};
        cfg.event_capacity = 1u << 20;
        cfg.runtime.coordinator.mode = arc::RuntimeMode::ObserveOnly;
        cfg.runtime.coordinator.max_actions_per_tick = 1;
        cfg.runtime.coordinator.max_budget_age_ticks = 240;
        cfg.runtime.governor.enable_memory = false;
        cfg.runtime.governor.enable_quality = true;
        cfg.runtime.governor.quality.overload_samples_required = 3;
        cfg.runtime.governor.quality.headroom_samples_required = 8;
        cfg.runtime.governor.quality.settle_samples_after_change = 4;
        cfg.runtime.governor.quality.minimum_hold_samples_after_degrade = 32;
        cfg.runtime.governor.quality.restore_probe_samples_required = 128;
        cfg.runtime.governor.quality.restore_probe_headroom_fraction = 0.18;
        cfg.runtime.governor.quality.restore_reversal_window_samples = 160;
        cfg.runtime.governor.quality.restore_backoff_base_samples = 128;
        cfg.runtime.governor.quality.restore_backoff_max_samples = 2048;
        cfg.runtime.governor.quality.frame_ewma_alpha = 0.35;
        cfg.runtime.governor.quality.overload_margin_ms = 0.03;
        cfg.runtime.governor.quality.extra_restore_headroom_ms = 0.10;
        cfg.runtime.governor.quality.optimizer.minimum_gain_ms = 0.02;
        cfg.runtime.governor.quality.optimizer.minimum_confidence = 0.35;
        cfg.runtime.governor.quality.optimizer.restoration_headroom_ms = 0.15;

        host_ = std::make_unique<arc::dx12::NativeHostAdapter>(device, nullptr, cfg);
        host_->set_mode(arc::RuntimeMode::ObserveOnly);
        host_->set_quality_mutator([this](const arc::QualityActionCandidate& action, bool restore)
        {
            return Mutate(action, restore);
        });

        resource_heap_ = resource_heap;
        sampler_heap_ = sampler_heap;
        if (resource_heap_) (void)host_->observe_descriptor_heap(resource_heap_);
        if (sampler_heap_) (void)host_->observe_descriptor_heap(sampler_heap_);


    }

    void ResourceCreated(ID3D12Resource* resource) noexcept
    {
        if (!host_ || !resource) return;
        (void)host_->observe_external_resource(resource);
    }

    void ResourceDestroyed(ID3D12Resource* resource) noexcept
    {
        if (!host_ || !resource) return;
        if (host_->resource_id(resource)) (void)host_->observe_resource_destroyed(resource);
        std::scoped_lock lock(truth_mutex_); truth_live_.erase(resource);
    }

    void ObserveSRV(
        ID3D12DescriptorHeap* heap,
        std::uint32_t index,
        ID3D12Resource* resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC* view) noexcept
    {
        if (!host_ || !heap || !resource || !view || index == UINT32_MAX) return;
        if (!host_->resource_id(resource)) (void)host_->observe_external_resource(resource);
        (void)host_->observe_srv(heap, index, resource, *view);
    }

    void ObserveUAV(
        ID3D12DescriptorHeap* heap,
        std::uint32_t index,
        ID3D12Resource* resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* view) noexcept
    {
        if (!host_ || !heap || !resource || !view || index == UINT32_MAX) return;
        if (!host_->resource_id(resource)) (void)host_->observe_external_resource(resource);
        (void)host_->observe_uav(heap, index, resource, *view);
    }

    void ObserveSampler(ID3D12DescriptorHeap* heap, std::uint32_t index) noexcept
    {
        if (!host_ || !heap || index == UINT32_MAX) return;
        (void)host_->observe_sampler(heap, index);
    }

    void CommandBegin(ID3D12CommandList* command, D3D12_COMMAND_LIST_TYPE type) noexcept
    {
        if (!host_ || !command) return;
        (void)host_->observe_command_list(command, type);
        (void)host_->observe_command_list_reset(command);
        std::scoped_lock lock(pending_mutex_);
        pending_[command] = {};
    }

    void CommandDestroyed(ID3D12CommandList* command) noexcept {
        if (!host_ || !command) return;
        { std::scoped_lock lock(pending_mutex_); pending_.erase(command); }
        if (host_->command_id(command)) (void)host_->observe_command_list_destroyed(command);
    }
    void ResourceTruth(ID3D12Resource* resource, const char* name) {
        if (!host_ || !resource || !name) return;
        const auto id=host_->resource_id(resource);
        const int entry=arc_wicked::audit::find(name);
        if (!id || entry<0) return;
        std::scoped_lock lock(truth_mutex_);
        if (truth_live_.size() >= 4096 && !truth_live_.contains(resource)) { truth_truncated_=true; return; }
        truth_live_[resource]={id,entry};
    }
    void QueueDestroyed(ID3D12CommandQueue* queue) noexcept {
        if (!host_ || !queue) return;
        if (host_->queue_id(queue) && !host_->observe_queue_destroyed(queue)) return;
        std::scoped_lock lock(queue_mutex_); queues_.erase(queue);
    }

    void ResourceUse(ID3D12CommandList* command, ID3D12Resource* resource, bool write) noexcept
    {
        if (!host_ || !command || !resource) return;
        if (!host_->resource_id(resource)) (void)host_->observe_external_resource(resource);
        std::scoped_lock lock(pending_mutex_);
        auto& current = pending_[command].uses[resource];
        current.observe(write);
    }

    void Transition(
        ID3D12CommandList* command,
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before_state,
        D3D12_RESOURCE_STATES after_state,
        std::uint32_t subresource) noexcept
    {
        if (!host_ || !command || !resource || before_state == after_state) return;
        if (!host_->resource_id(resource)) (void)host_->observe_external_resource(resource);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before_state;
        barrier.Transition.StateAfter = after_state;
        barrier.Transition.Subresource = subresource;
        (void)host_->observe_transition_barrier(command, resource, barrier);
    }

    void Copy(
        ID3D12CommandList* command,
        ID3D12Resource* source,
        ID3D12Resource* destination,
        std::uint64_t bytes,
        std::uint32_t kind) noexcept
    {
        if (!host_ || !command || !source || !destination) return;
        if (!host_->resource_id(source)) (void)host_->observe_external_resource(source);
        if (!host_->resource_id(destination)) (void)host_->observe_external_resource(destination);
        if (kind == 1) (void)host_->observe_copy_buffer(command, source, destination, bytes);
        else if (kind == 2) (void)host_->observe_copy_texture(command, source, destination, bytes);
        else (void)host_->observe_copy_resource(command, source, destination, bytes);
    }

    void Count(ID3D12CommandList* command, std::uint32_t kind, std::uint64_t work_items) noexcept
    {
        if (!command) return;
        std::scoped_lock lock(pending_mutex_);
        auto& counters = pending_[command].counters;
        switch (kind)
        {
        case 0:
            ++counters.draws;
            counters.draw_items += work_items;
            break;
        case 1:
            ++counters.indexed_draws;
            counters.draw_items += work_items;
            break;
        case 2:
            ++counters.dispatches;
            counters.dispatch_groups += work_items;
            break;
        default:
            ++counters.indirect;
            break;
        }
    }

    void Submit(
        ID3D12CommandQueue* queue,
        ID3D12CommandList* const* commands,
        std::size_t count,
        D3D12_COMMAND_LIST_TYPE type) noexcept
    {
        if (!host_ || !queue || !commands || count == 0) return;
        (void)host_->observe_queue(queue, type);

        for (std::size_t i = 0; i < count; ++i)
        {
            ID3D12CommandList* command = commands[i];
            CommandPending state{};
            {
                std::scoped_lock lock(pending_mutex_);
                if (const auto it = pending_.find(command); it != pending_.end())
                {
                    state = std::move(it->second);
                    pending_.erase(it);
                }
            }

            for (const auto& [resource, access] : state.uses)
            {
                if (access.read) (void)host_->observe_resource_use(command, resource, false);
                if (access.write) (void)host_->observe_resource_use(command, resource, true);
            }

            const auto& c = state.counters;
            if (c.draws || c.indexed_draws || c.dispatches || c.indirect)
                (void)host_->observe_command_counters(
                    command, c.draws, c.indexed_draws, c.dispatches, c.indirect,
                    c.draw_items, c.dispatch_groups);

            (void)host_->observe_command_list_closed(command);
        }

        (void)host_->observe_execute_command_lists(
            queue, std::span<ID3D12CommandList* const>(commands, count));

        // Wicked quality setters own their renderer synchronization. Extra
        // ARC completion fences are only needed for explicitly opted-in
        // residency resources; observation must not inject GPU queue work.
        if (host_->metrics().controlled_resources == 0) return;

        Microsoft::WRL::ComPtr<ID3D12Fence> completion_fence;
        std::uint64_t completion_value = 0;
        {
            std::scoped_lock lock(queue_mutex_);
            auto [it, inserted] = queues_.try_emplace(queue);
            auto& queue_state = it->second;
            queue_state.type = type;
            if (inserted && device_)
            {
                if (SUCCEEDED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue_state.fence))))
                    (void)host_->bind_completion_fence(queue, queue_state.fence.Get());
            }
            if (queue_state.fence)
            {
                completion_fence = queue_state.fence;
                completion_value = ++queue_state.value;
            }
        }

        if (completion_fence && SUCCEEDED(queue->Signal(completion_fence.Get(), completion_value)))
        {
            (void)host_->observe_fence_signal(queue, completion_fence.Get(), completion_value);
            (void)host_->poll_completion(queue);
        }
    }

    void Present(
        std::uint64_t swapchain_id,
        std::uint32_t sync_interval,
        std::uint32_t flags,
        HRESULT result) noexcept
    {
        if (!host_) return;
        const auto arc_begin = std::chrono::steady_clock::now();
        if (FAILED(result))
        {
            ++present_failures_;
            last_present_failure_ = result;
            device_removed_reason_ = device_ ? device_->GetDeviceRemovedReason() : result;
            WriteDeviceLossContext(last_present_failure_, device_removed_reason_);
        }
        (void)host_->observe_present(swapchain_id, sync_interval, flags, result);
        (void)host_->poll_all_completions();
        (void)host_->drain();
        const auto arc_end = std::chrono::steady_clock::now();
        arc_present_cost_ms_.push_back(
            std::chrono::duration<double, std::milli>(arc_end - arc_begin).count());
    }

    void HarnessUpdate(wi::gui::ComboBox& selector, std::uint32_t width, std::uint32_t height) noexcept
    {
        if (phase_ == Phase::Done) return;
        if (cpu_profile_.is_open()) {
            ++cpu_profile_frame_;
            g_arc_last_scene.store(cpu_profile_scene_, std::memory_order_relaxed);
            if (!cpu_profile_selected_) {
                ForceFullQuality();
                const auto begin = std::chrono::steady_clock::now();
                selector.SetSelected(cpu_profile_scene_);
                CpuSample("Scene switch", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
                cpu_profile_selected_ = true;
            }
            wi::renderer::SetTemporalAAEnabled(false);
            wi::profiler::SetEnabled(true);
            return;
        }
        if (experiment_mode_ == "off" || experiment_mode_ == "observe" || experiment_mode_ == "adaptive")
        {
            ExperimentUpdate(selector, width, height);
            return;
        }
        if (!host_) return;

        g_arc_last_phase.store(static_cast<std::uint32_t>(phase_), std::memory_order_relaxed);
        g_arc_last_scene.store(static_cast<std::uint32_t>(current_scene_), std::memory_order_relaxed);
        width_ = width;
        height_ = height;
        wi::renderer::SetTemporalAAEnabled(false);
        wi::profiler::SetEnabled(true);

        const auto now = std::chrono::steady_clock::now();
        if (have_last_harness_update_)
        {
            cpu_frame_intervals_ms_.push_back(
                std::chrono::duration<double, std::milli>(now - last_harness_update_).count());
        }
        last_harness_update_ = now;
        have_last_harness_update_ = true;
        if (phase_ == Phase::Dormant)
        {
            ForceFullQuality();
            phase_ = Phase::Warmup;
            phase_start_ = now;
            current_scene_ = 0;
            selector.SetSelected(kScenes[current_scene_].combo_index);
            AfterSceneSwitch(now);
            return;
        }

        const double gpu_ms = static_cast<double>(wi::profiler::GetLastGPUFrameTimeMS());
        const double elapsed = std::chrono::duration<double>(now - phase_start_).count();

        if (phase_ == Phase::Warmup)
        {
            // Prewarm every measured scene before collecting baseline data.
            // Otherwise baseline sees cold asset/resource/shader state while
            // adaptive sees the same scenes already resident/cached, which
            // makes per-scene comparisons order-dependent.
            const double slot_s = static_cast<double>(warmup_seconds_);
            const std::size_t desired = std::min<std::size_t>(
                static_cast<std::size_t>(elapsed / slot_s), kScenes.size());

            if (desired >= kScenes.size())
            {
                EnterMeasuredPhase(Phase::Baseline, selector, now);
                return;
            }

            if (desired != current_scene_)
            {
                current_scene_ = desired;
                selector.SetSelected(kScenes[current_scene_].combo_index);
                AfterSceneSwitch(now);
            }
            return;
        }

        if (phase_ == Phase::Baseline || phase_ == Phase::Adaptive)
        {
            const double active_per_scene = static_cast<double>(seconds_) / static_cast<double>(kScenes.size());
            const double settle_s = static_cast<double>(settle_milliseconds_) / 1000.0;
            const double slot_s = active_per_scene + settle_s;
            const std::size_t desired = std::min<std::size_t>(
                static_cast<std::size_t>(elapsed / slot_s), kScenes.size());

            if (desired >= kScenes.size())
            {
                CaptureSceneSemantic(phase_, current_scene_);
                if (phase_ == Phase::Baseline) FinishBaseline(selector, now);
                else FinishAdaptive(selector, now);
                return;
            }

            if (desired != current_scene_)
            {
                CaptureSceneSemantic(phase_, current_scene_);
                current_scene_ = desired;
                selector.SetSelected(kScenes[current_scene_].combo_index);
                AfterSceneSwitch(now);
                return;
            }

            const double within_slot = elapsed - static_cast<double>(current_scene_) * slot_s;
            if (within_slot < settle_s || gpu_ms <= 0.0) return;

            if (!scene_semantic_window_started_)
            {
                (void)host_->drain();
                scene_checkpoint_ = scene_understanding_.checkpoint(host_->graph());
                scene_semantic_window_started_ = true;
                return;
            }

            if (phase_ == Phase::Baseline)
            {
                AddSample(baseline_, baseline_scenes_[current_scene_], gpu_ms);
            }
            else
            {
                AddSample(adaptive_, adaptive_scenes_[current_scene_], gpu_ms);
                control_window_.push_back(gpu_ms);
                if (static_cast<int>(control_window_.size()) >= control_frames_)
                {
                    TickGovernor(target_ms_);
                    ++adaptive_ticks_;
                }
            }
            return;
        }

        if (phase_ == Phase::Recovery)
        {
            if (gpu_ms > 0.0)
            {
                control_window_.push_back(gpu_ms);
                if (static_cast<int>(control_window_.size()) >= control_frames_)
                {
                    TickGovernor(recovery_target_ms_);
                    ++recovery_ticks_;
                }
            }

            const auto quality = host_->runtime().governor().quality().state();
            if ((FullQuality() && quality.active_actions == 0 && recovery_ticks_ >= 2) ||
                elapsed >= static_cast<double>(recovery_seconds_))
            {
                Finalize();
                phase_ = Phase::Done;
                PostQuitMessage(0);
            }
        }
    }

    bool Finished() const noexcept { return phase_ == Phase::Done; }

    void CpuSample(const char* name, double milliseconds) noexcept {
        if (!cpu_profile_.is_open() || cpu_profile_rows_ >= 100000 || !std::isfinite(milliseconds)) return;
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpu_profile_start_).count();
        cpu_profile_ << std::setprecision(9) << elapsed << ',' << cpu_profile_frame_ << ',' << cpu_profile_scene_ << ",\"";
        for (const char* p = name; *p; ++p) { if (*p == '"') cpu_profile_ << '"'; cpu_profile_ << *p; }
        cpu_profile_ << "\"," << milliseconds << '\n';
        ++cpu_profile_rows_;
    }

    void PollCpuProfile() noexcept {
        if (!cpu_profile_.is_open()) return;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - cpu_profile_start_).count() < cpu_profile_seconds_) return;
        cpu_profile_.flush();
        cpu_profile_.close();
        phase_ = Phase::Done;
        PostQuitMessage(0);
    }

    void ForceFullQuality() noexcept
    {
        shadow_level_ = 0;
        geometry_level_ = 0;
        bandwidth_level_ = 0;
        ApplyCurrentQuality();
    }

private:
    // Independent OFF/observer/controlled experiment with a shared calibration.
    // Does not produce stage acceptance or validate recovery/image quality.
    void ExperimentUpdate(wi::gui::ComboBox& selector, std::uint32_t width, std::uint32_t height)
    {
        const auto now = std::chrono::steady_clock::now();
        width_ = width;
        height_ = height;
        wi::renderer::SetTemporalAAEnabled(false);
        wi::profiler::SetEnabled(true);
        const double cpu_ms = have_last_harness_update_ ?
            std::chrono::duration<double, std::milli>(now - last_harness_update_).count() : 0.0;
        last_harness_update_ = now;
        have_last_harness_update_ = true;
        if (phase_ == Phase::Dormant)
        {
            ForceFullQuality();
            phase_ = Phase::Warmup;
            phase_start_ = now;
            current_scene_ = 0;
            selector.SetSelected(kScenes[scene_offset_].combo_index);
            AfterSceneSwitch(now);
            return;
        }
        const double elapsed = std::chrono::duration<double>(now - phase_start_).count();
        const double settle_s = static_cast<double>(settle_milliseconds_) / 1000.0;
        const double slot_s = phase_ == Phase::Warmup ? static_cast<double>(warmup_seconds_) :
            static_cast<double>(seconds_) / kScenes.size() + settle_s;
        const auto desired = std::min<std::size_t>(static_cast<std::size_t>(elapsed / slot_s), kScenes.size());
        if (desired == kScenes.size())
        {
            if (phase_ == Phase::Warmup)
            {
                if (experiment_mode_ == "adaptive")
                {
                    // Explicit microseconds avoid locale-dependent decimal parsing.
                    target_ms_ = EnvInt(L"ARC_WICKED_TARGET_US", 0, 0, 1000000) / 1000.0;
                    const double p50 = EnvInt(L"ARC_WICKED_BASELINE_P50_US", 0, 0, 1000000) / 1000.0;
                    if (target_ms_ <= 0.0 || p50 <= 0.0) { phase_ = Phase::Done; PostQuitMessage(2); return; }
                    RegisterQualityProfiles(p50);
                    if (!profiles_registered_) { phase_ = Phase::Done; PostQuitMessage(3); return; }
                    host_->runtime().governor().quality().reset();
                    host_->set_mode(arc::RuntimeMode::Controlled);
                    last_control_metrics_ = host_->metrics();
                }
                phase_ = Phase::Baseline;
                phase_start_ = now;
                current_scene_ = 0;
                selector.SetSelected(kScenes[scene_offset_].combo_index);
                AfterSceneSwitch(now);
                return;
            }
            if (!output_path_.parent_path().empty()) std::filesystem::create_directories(output_path_.parent_path());
            if (host_) (void)host_->drain();
            const auto hm = host_ ? host_->metrics() : arc::dx12::NativeHostAdapterMetrics{};
            const auto graph_errors = host_ ? host_->graph().errors() : 0;
            const auto backend_failures = host_ ? host_->runtime().governor().metrics().quality_backend_failures : 0;
            const auto removed = device_ ? device_->GetDeviceRemovedReason() : E_FAIL;
            const bool experiment_valid = hm.failed_observations == 0 && hm.bridge_rejections == 0 &&
                graph_errors == 0 && backend_failures == 0 && invalid_samples_ == 0 &&
                g_present_failures.load(std::memory_order_relaxed) == 0 && SUCCEEDED(removed) &&
                (experiment_mode_ != "adaptive" || profiles_registered_);
            std::ofstream out(output_path_, std::ios::trunc);
            out << std::setprecision(9)
                << "{\"schema\":1,\"experiment\":\"three_arm_comparison\",\"acceptance_evaluated\":false,"
                << "\"valid\":" << (experiment_valid ? "true" : "false") << ','
                << "\"mode\":\"" << experiment_mode_ << "\",\"arc_source_sha\":\"" << arc_source_sha_
                << "\",\"scene_offset\":" << scene_offset_ << ",\"width\":" << width_
                << ",\"height\":" << height_
                << ",\"target_ms\":" << target_ms_
                << ",\"calibration_p40_ms\":" << Percentile(baseline_.frames, 0.40)
                << ",\"calibration_p50_ms\":" << Percentile(baseline_.frames, 0.50)
                << ",\"quality_actions\":" << (host_ ? host_->runtime().governor().metrics().quality_actions_executed : 0)
                << ",\"graph_errors\":" << graph_errors
                << ",\"failed_observations\":" << hm.failed_observations
                << ",\"bridge_rejections\":" << hm.bridge_rejections
                << ",\"backend_failures\":" << backend_failures
                << ",\"invalid_samples\":" << invalid_samples_
                << ",\"present_failures\":" << g_present_failures.load(std::memory_order_relaxed)
                << ",\"device_removed_reason\":" << static_cast<std::int32_t>(removed)
                << ",\"recovery_validated\":false,\"scenes\":[";
            for (std::size_t i = 0; i < kScenes.size(); ++i)
            {
                if (i) out << ',';
                out << "{\"name\":\"" << kScenes[i].name << "\",\"gpu_samples\":" << baseline_scenes_[i].frames.size()
                    << ",\"cpu_samples\":" << experiment_cpu_[i].frames.size()
                    << ",\"gpu_p50_ms\":" << Percentile(baseline_scenes_[i].frames, 0.50)
                    << ",\"gpu_p95_ms\":" << Percentile(baseline_scenes_[i].frames, 0.95)
                    << ",\"cpu_p50_ms\":" << Percentile(experiment_cpu_[i].frames, 0.50)
                    << ",\"cpu_p95_ms\":" << Percentile(experiment_cpu_[i].frames, 0.95) << '}';
            }
            out << "],\"hook_timings\":";
            WriteHookTimings(out);
            out << "}\n";
            // This diagnostic resets renderer settings; it does not validate
            // governor recovery. The official stage harness does that separately.
            ForceFullQuality();
            phase_ = Phase::Done;
            PostQuitMessage(0);
            return;
        }
        const auto scene = (desired + scene_offset_) % kScenes.size();
        if (desired != current_scene_)
        {
            current_scene_ = desired;
            selector.SetSelected(kScenes[scene].combo_index);
            AfterSceneSwitch(now);
            return;
        }
        if (phase_ == Phase::Warmup || elapsed - desired * slot_s < settle_s) return;
        const double gpu_ms = static_cast<double>(wi::profiler::GetLastGPUFrameTimeMS());
        if (!std::isfinite(gpu_ms) || !std::isfinite(cpu_ms)) { ++invalid_samples_; return; }
        if (gpu_ms > 0.0)
        {
            baseline_scenes_[scene].frames.push_back(gpu_ms);
            baseline_.frames.push_back(gpu_ms);
            if (experiment_mode_ == "adaptive")
            {
                control_window_.push_back(gpu_ms);
                if (static_cast<int>(control_window_.size()) >= control_frames_) TickGovernor(target_ms_);
            }
        }
        if (cpu_ms > 0.0) experiment_cpu_[scene].frames.push_back(cpu_ms);
    }

    void CaptureSceneSemantic(Phase phase, std::size_t scene)
    {
        if (!host_ || scene >= kScenes.size() ||
            (phase != Phase::Baseline && phase != Phase::Adaptive) ||
            !scene_semantic_window_started_)
            return;

        auto& capture = phase == Phase::Baseline ?
            baseline_scene_semantics_[scene] : adaptive_scene_semantics_[scene];
        if (capture.captured) return;

        (void)host_->drain();
        capture.signature = scene_understanding_.summarize(host_->graph(), scene_checkpoint_);
        // Independent source-owner labels join only AFTER inference. First
        // baseline observation per resource ID avoids repeated-frame weighting.
        if (phase == Phase::Baseline) {
            std::scoped_lock lock(truth_mutex_);
            for(const auto& [pointer, truth] : truth_live_) {
                (void)pointer;
                auto resource=host_->graph().find(truth.first);
                if(!resource || !resource->alive || truth_samples_.contains(truth.first)) continue;
                if(!resource->read_count && !resource->write_count) continue;
                auto prediction=arc::ResourceSemanticInferencer{}.classify(host_->graph(),truth.first);
                if(truth_samples_.size()>=4096) { truth_truncated_=true; break; }
                truth_samples_.emplace(truth.first,AuditSample{truth.second,prediction.semantic,prediction.confidence});
            }
        }
        capture.cluster = scene_clusters_.observe(capture.signature);
        capture.captured = true;
    }

    void AddSample(Stats& total, Stats& scene, double ms)
    {
        total.frames.push_back(ms);
        scene.frames.push_back(ms);
        if (target_ms_ > 0.0 && ms > target_ms_)
        {
            ++total.misses;
            total.overshoot += ms - target_ms_;
            ++scene.misses;
            scene.overshoot += ms - target_ms_;
        }
    }

    void ApplyCurrentQuality() noexcept
    {
        constexpr std::array<int, 4> res2d{2048, 1536, 1024, 512};
        constexpr std::array<int, 4> cube{1024, 768, 512, 256};
        wi::renderer::SetShadowProps2D(res2d[shadow_level_]);
        wi::renderer::SetShadowPropsCube(cube[shadow_level_]);
        wi::renderer::SetTessellationEnabled(geometry_level_ == 0);
        constexpr std::array<float, 3> mip_bias{0.0f, 0.75f, 1.50f};
        if (const auto* object_sampler = wi::renderer::GetSampler(wi::enums::SAMPLER_OBJECTSHADER))
        {
            auto sampler_desc = object_sampler->GetDesc();
            sampler_desc.mip_lod_bias = mip_bias[bandwidth_level_];
            wi::renderer::ModifyObjectSampler(sampler_desc);
        }
        wi::renderer::SetTemporalAAEnabled(false);
    }

    void AfterSceneSwitch(std::chrono::steady_clock::time_point now)
    {
        settle_until_ = now + std::chrono::milliseconds(settle_milliseconds_);
        ApplyCurrentQuality();
        wi::profiler::SetEnabled(true);
        control_window_.clear();
        scene_semantic_window_started_ = false;
        scene_checkpoint_ = {};
    }

    void EnterMeasuredPhase(
        Phase phase,
        wi::gui::ComboBox& selector,
        std::chrono::steady_clock::time_point now)
    {
        phase_ = phase;
        phase_start_ = now;
        current_scene_ = 0;
        selector.SetSelected(kScenes[0].combo_index);
        AfterSceneSwitch(now);
    }

    void FinishBaseline(
        wi::gui::ComboBox& selector,
        std::chrono::steady_clock::time_point now)
    {
        target_ms_ = std::max(0.05, Percentile(baseline_.frames, 0.40));
        const double baseline_p50 = std::max(0.05, Percentile(baseline_.frames, 0.50));
        recovery_target_ms_ = std::max(target_ms_ * 1.60, baseline_p50 * 1.35);
        Recompute(baseline_, target_ms_);
        for (auto& x : baseline_scenes_) Recompute(x, target_ms_);

        RegisterQualityProfiles(baseline_p50);
        host_->runtime().governor().quality().reset();
        host_->set_mode(arc::RuntimeMode::Controlled);
        last_control_metrics_ = host_->metrics();
        control_window_.clear();
        EnterMeasuredPhase(Phase::Adaptive, selector, now);
    }

    void FinishAdaptive(
        wi::gui::ComboBox& selector,
        std::chrono::steady_clock::time_point now)
    {
        // Evaluate the renderer population while the measured workload is
        // still loaded. Recovery switches to HelloWorld and retires resources;
        // its eventual live count is not evidence about the measured workload.
        (void)host_->drain();
        measured_semantics_ = arc::ResourceSemanticInferencer{}.classify_all(host_->graph(), true);
        semantic_snapshot_frame_ = host_->graph().presentation_frame();
        const auto gm = host_->runtime().governor().metrics();
        const auto qs = host_->runtime().governor().quality().state();
        adaptive_quality_actions_ = gm.quality_actions_executed;
        adaptive_direction_changes_ = qs.direction_changes;
        adaptive_restore_probes_ = qs.restore_probes;
        adaptive_restore_backoffs_ = qs.restore_backoffs;

        host_->runtime().governor().quality().begin_recovery();
        phase_ = Phase::Recovery;
        phase_start_ = now;
        current_scene_ = 0;
        selector.SetSelected(0);
        AfterSceneSwitch(now);
        control_window_.clear();
    }

    void RegisterQualityProfiles(double baseline_p50)
    {
        if (profiles_registered_) return;

        arc::QualityResourceProfile shadow{};
        shadow.id = kShadowProfile;
        shadow.domain = arc::QualityDomain::Shadow;
        shadow.label = "wicked shadow resolution";
        shadow.semantic = arc::QualitySemanticClass::Environment;
        shadow.importance = {0.70, 1.0, 0.35, 0.55, 0.55};
        shadow.confidence = 0.92;
        shadow.reversible = true;
        shadow.levels = {
            {0.75, baseline_p50 * 0.08, 0},
            {0.50, baseline_p50 * 0.07, 0},
            {0.25, baseline_p50 * 0.05, 0},
        };

        arc::QualityResourceProfile geometry{};
        geometry.id = kGeometryProfile;
        geometry.domain = arc::QualityDomain::Geometry;
        geometry.label = "wicked tessellation";
        geometry.semantic = arc::QualitySemanticClass::Environment;
        geometry.importance = {0.60, 0.95, 0.45, 0.45, 0.55};
        geometry.confidence = 0.78;
        geometry.reversible = true;
        geometry.levels = {
            {0.55, baseline_p50 * 0.09, 0},
        };

        arc::QualityResourceProfile bandwidth{};
        bandwidth.id = kBandwidthProfile;
        bandwidth.domain = arc::QualityDomain::Bandwidth;
        bandwidth.label = "wicked material mip bias";
        bandwidth.semantic = arc::QualitySemanticClass::Environment;
        bandwidth.importance = {0.65, 0.95, 0.40, 0.45, 0.60};
        bandwidth.confidence = 0.82;
        bandwidth.reversible = true;
        bandwidth.levels = {
            {0.75, baseline_p50 * 0.07, 0},
            {0.50, baseline_p50 * 0.06, 0},
        };

        profiles_registered_ =
            host_->register_quality_profile(shadow) &&
            host_->register_quality_profile(geometry) &&
            host_->register_quality_profile(bandwidth);
    }

    void TickGovernor(double target)
    {
        if (control_window_.empty()) return;

        const auto metrics = host_->metrics();
        const double draws = static_cast<double>(
            Delta(metrics.draws_observed, last_control_metrics_.draws_observed) +
            Delta(metrics.indexed_draws_observed, last_control_metrics_.indexed_draws_observed));
        const double dispatches = static_cast<double>(
            Delta(metrics.dispatches_observed, last_control_metrics_.dispatches_observed));
        const double uses = static_cast<double>(
            Delta(metrics.resource_uses, last_control_metrics_.resource_uses));
        const double descriptors = static_cast<double>(
            Delta(metrics.descriptor_writes, last_control_metrics_.descriptor_writes));
        const double barriers = static_cast<double>(
            Delta(metrics.barriers_observed, last_control_metrics_.barriers_observed));

        last_control_metrics_ = metrics;

        const double draw_pressure = std::clamp(draws / 1800.0, 0.0, 1.0);
        const double dispatch_pressure = std::clamp(dispatches / 320.0, 0.0, 1.0);
        const double use_pressure = std::clamp(uses / 2500.0, 0.0, 1.0);
        const double descriptor_pressure = std::clamp(descriptors / 160.0, 0.0, 1.0);
        const double barrier_pressure = std::clamp(barriers / 600.0, 0.0, 1.0);

        arc::FrameBudgetSample frame{};
        frame.frame_ms = Percentile(control_window_, 0.50);
        frame.target_frame_ms = target;
        frame.gpu_busy_fraction = std::clamp(frame.frame_ms / std::max(0.001, target), 0.0, 1.0);
        // Keep the class-specific signals conservative. When the renderer is
        // broadly GPU-bound, UnknownGpu should win over a guessed semantic
        // bottleneck so ARC may compare all non-temporal quality domains.
        // Stage 15 will replace these activity heuristics with automatic scene
        // understanding.
        frame.memory_bandwidth_fraction = std::min(0.70, std::max(use_pressure, descriptor_pressure));
        frame.geometry_pressure = std::min(0.70, draw_pressure);
        frame.lighting_pressure = std::min(0.70, std::max(dispatch_pressure, draw_pressure * 0.72));
        frame.shadow_pressure = std::min(0.70, std::max(barrier_pressure, draw_pressure * 0.82));
        frame.raster_pressure = std::min(
            0.70, std::clamp(draw_pressure * 0.55 + use_pressure * 0.45, 0.0, 1.0));

        control_window_.clear();
        const auto arc_tick_begin = std::chrono::steady_clock::now();
        (void)host_->frame_tick(frame, false);
        const auto arc_tick_end = std::chrono::steady_clock::now();
        arc_tick_cost_ms_.push_back(
            std::chrono::duration<double, std::milli>(arc_tick_end - arc_tick_begin).count());
    }

    arc::RuntimeBackendStatus Mutate(
        const arc::QualityActionCandidate& action,
        bool restore) noexcept
    {
        if (action.id == kShadowProfile)
        {
            constexpr std::array<int, 4> res2d{2048, 1536, 1024, 512};
            constexpr std::array<int, 4> cube{1024, 768, 512, 256};
            if (!restore)
            {
                if (action.sequence != shadow_level_ || shadow_level_ + 1 >= res2d.size())
                    return arc::RuntimeBackendStatus::Failure;
                ++shadow_level_;
            }
            else
            {
                if (shadow_level_ == 0 || action.sequence + 1 != shadow_level_)
                    return arc::RuntimeBackendStatus::Failure;
                --shadow_level_;
            }
            wi::renderer::SetShadowProps2D(res2d[shadow_level_]);
            wi::renderer::SetShadowPropsCube(cube[shadow_level_]);
            quality_domains_[0] = true;
        }
        else if (action.id == kGeometryProfile)
        {
            if (!restore)
            {
                if (action.sequence != 0 || geometry_level_ != 0)
                    return arc::RuntimeBackendStatus::Failure;
                geometry_level_ = 1;
                wi::renderer::SetTessellationEnabled(false);
            }
            else
            {
                if (geometry_level_ != 1)
                    return arc::RuntimeBackendStatus::Failure;
                geometry_level_ = 0;
                wi::renderer::SetTessellationEnabled(true);
            }
            quality_domains_[1] = true;
        }
        else if (action.id == kBandwidthProfile)
        {
            constexpr std::array<float, 3> mip_bias{0.0f, 0.75f, 1.50f};
            if (!restore)
            {
                if (action.sequence != bandwidth_level_ || bandwidth_level_ + 1 >= mip_bias.size())
                    return arc::RuntimeBackendStatus::Failure;
                ++bandwidth_level_;
            }
            else
            {
                if (bandwidth_level_ == 0 || action.sequence + 1 != bandwidth_level_)
                    return arc::RuntimeBackendStatus::Failure;
                --bandwidth_level_;
            }

            if (const auto* object_sampler = wi::renderer::GetSampler(wi::enums::SAMPLER_OBJECTSHADER))
            {
                auto sampler_desc = object_sampler->GetDesc();
                sampler_desc.mip_lod_bias = mip_bias[bandwidth_level_];
                wi::renderer::ModifyObjectSampler(sampler_desc);
            }
            quality_domains_[2] = true;
        }
        else
        {
            return arc::RuntimeBackendStatus::Unsupported;
        }

        wi::renderer::SetTemporalAAEnabled(false);
        return arc::RuntimeBackendStatus::Success;
    }

    bool FullQuality() const noexcept
    {
        return shadow_level_ == 0 && geometry_level_ == 0 && bandwidth_level_ == 0;
    }

    void Finalize()
    {
        if (!host_) return;
        (void)host_->poll_all_completions();
        (void)host_->drain();

        const auto hm = host_->metrics();
        const auto gm = host_->runtime().governor().metrics();
        const auto qs = host_->runtime().governor().quality().state();
        const auto bm = host_->runtime().bridge().metrics();

        std::uint64_t learned = 0;
        for (const auto id : {kShadowProfile, kGeometryProfile, kBandwidthProfile})
        {
            arc::QualityResourceProfile probe{};
            probe.id = id;
            if (id == kShadowProfile)
            {
                probe.domain = arc::QualityDomain::Shadow;
                probe.levels = {{0.75, 0.1, 0}, {0.5, 0.1, 0}, {0.25, 0.1, 0}};
            }
            else
            {
                probe.domain = id == kGeometryProfile ? arc::QualityDomain::Geometry : arc::QualityDomain::Bandwidth;
                probe.levels = id == kGeometryProfile
                    ? std::vector<arc::QualityLevelStep>{{0.5, 0.1, 0}}
                    : std::vector<arc::QualityLevelStep>{{0.75, 0.1, 0}, {0.5, 0.1, 0}};
            }
            for (const auto& action : arc::QualityCandidateFactory::build(probe))
                if (host_->runtime().governor().quality().effects().find(action)) ++learned;
        }

        const double bp50 = Percentile(baseline_.frames, 0.50);
        const double ap50 = Percentile(adaptive_.frames, 0.50);
        const double bp99 = Percentile(baseline_.frames, 0.99);
        const double ap99 = Percentile(adaptive_.frames, 0.99);
        const double bmiss = MissRatio(baseline_);
        const double amiss = MissRatio(adaptive_);
        const double miss_reduction = bmiss > 0.0 ? (bmiss - amiss) / bmiss : 0.0;
        const double p50_reduction = bp50 > 0.0 ? (bp50 - ap50) / bp50 : 0.0;

        std::uint64_t scene_pairs = 0;
        std::uint64_t scene_wins = 0;
        for (std::size_t i = 0; i < kScenes.size(); ++i)
        {
            if (baseline_scenes_[i].frames.size() >= 50 && adaptive_scenes_[i].frames.size() >= 50)
            {
                ++scene_pairs;
                const double b = Percentile(baseline_scenes_[i].frames, 0.50);
                const double a = Percentile(adaptive_scenes_[i].frames, 0.50);
                if (b > 0.0 && a <= b * 0.97) ++scene_wins;
            }
        }

        std::uint64_t domains = 0;
        for (const bool x : quality_domains_) if (x) ++domains;

        const double action_rate = adaptive_ticks_ ?
            static_cast<double>(adaptive_quality_actions_) / static_cast<double>(adaptive_ticks_) : 0.0;
        const double direction_rate = adaptive_ticks_ ?
            static_cast<double>(adaptive_direction_changes_) / static_cast<double>(adaptive_ticks_) : 0.0;

        const bool host_clean =
            hm.failed_observations == 0 &&
            hm.bridge_rejections == 0 &&
            bm.controller_rejections == 0 &&
            bm.malformed_events == 0 &&
            host_->graph().errors() == 0;
        const bool timing_valid =
            baseline_.frames.size() >= 500 &&
            adaptive_.frames.size() >= 500;
        const bool baseline_pressure = bmiss >= 0.45 && bmiss <= 0.75;
        const bool complex_renderer =
            hm.resources_observed >= 128 &&
            hm.descriptor_writes >= 50 &&
            hm.queue_submits >= 500 &&
            (hm.draws_observed + hm.indexed_draws_observed) >= 1000;
        const bool scene_coverage = scene_pairs >= 4;
        const bool physical_quality = gm.quality_actions_executed >= 2 && domains >= 2;
        const bool learning = learned >= 2;
        const bool performance =
            baseline_pressure &&
            miss_reduction >= 0.10 &&
            p50_reduction >= 0.05 &&
            ap99 <= bp99 * 1.15 &&
            scene_wins >= 2;
        const bool bounded_churn =
            adaptive_quality_actions_ <= 32 &&
            action_rate <= 0.050 &&
            direction_rate <= 0.030;
        const bool full_recovery = FullQuality() && qs.active_actions == 0;
        const bool native_1080 = width_ == 1920 && height_ == 1080;

        const bool valid =
            host_clean && timing_valid && complex_renderer && scene_coverage &&
            physical_quality && learning && performance && bounded_churn &&
            full_recovery && native_1080 && profiles_registered_;

        const double cpu_frame_p50 = Percentile(cpu_frame_intervals_ms_, 0.50);
        const double cpu_frame_p99 = Percentile(cpu_frame_intervals_ms_, 0.99);
        const double cpu_frame_max = Percentile(cpu_frame_intervals_ms_, 1.00);
        const double arc_present_p50 = Percentile(arc_present_cost_ms_, 0.50);
        const double arc_present_p99 = Percentile(arc_present_cost_ms_, 0.99);
        const double arc_present_max = Percentile(arc_present_cost_ms_, 1.00);
        const double arc_tick_p50 = Percentile(arc_tick_cost_ms_, 0.50);
        const double arc_tick_p99 = Percentile(arc_tick_cost_ms_, 0.99);
        const double arc_tick_max = Percentile(arc_tick_cost_ms_, 1.00);
        const auto cpu_hitches_50ms = static_cast<std::uint64_t>(std::count_if(
            cpu_frame_intervals_ms_.begin(), cpu_frame_intervals_ms_.end(),
            [](double ms) { return ms >= 50.0; }));
        const auto cpu_hitches_100ms = static_cast<std::uint64_t>(std::count_if(
            cpu_frame_intervals_ms_.begin(), cpu_frame_intervals_ms_.end(),
            [](double ms) { return ms >= 100.0; }));

        const auto& semantic_predictions = measured_semantics_;
        std::array<std::uint64_t, arc::kInferredResourceSemanticCount> semantic_counts{};
        std::uint64_t semantic_known = 0;
        std::uint64_t semantic_high_confidence = 0;
        double semantic_confidence_sum = 0.0;
        for (const auto& prediction : semantic_predictions)
        {
            const auto index = static_cast<std::size_t>(prediction.semantic);
            if (index < semantic_counts.size()) ++semantic_counts[index];
            if (prediction.semantic != arc::InferredResourceSemantic::Unknown)
            {
                ++semantic_known;
                semantic_confidence_sum += prediction.confidence;
                if (prediction.confidence >= 0.75f) ++semantic_high_confidence;
            }
        }
        const double semantic_coverage = semantic_predictions.empty() ? 0.0 :
            static_cast<double>(semantic_known) / static_cast<double>(semantic_predictions.size());
        const double semantic_high_confidence_ratio = semantic_known == 0 ? 0.0 :
            static_cast<double>(semantic_high_confidence) / static_cast<double>(semantic_known);
        const double semantic_mean_confidence = semantic_known == 0 ? 0.0 :
            semantic_confidence_sum / static_cast<double>(semantic_known);

        std::error_code ec;
        if (const auto parent = output_path_.parent_path(); !parent.empty())
            std::filesystem::create_directories(parent, ec);

        std::ofstream f(output_path_, std::ios::trunc);
        if (!f) return;
        f << std::fixed << std::setprecision(6)
          << "{\n"
          << "  \"schema\":1,\n"
          << "  \"valid\":" << (valid ? "true" : "false") << ",\n"
          << "  \"benchmark\":\"stage14_5_wicked_engine\",\n"
          << "  \"integration\":\"wicked_engine_tests_dx12\",\n"
          << "  \"wicked_upstream_sha\":\"" << kWickedUpstreamSha << "\",\n"
          << "  \"arc_source_sha\":\"" << arc_source_sha_ << "\",\n"
          << "  \"native_width\":" << width_ << ",\n"
          << "  \"native_height\":" << height_ << ",\n"
          << "  \"temporal_used\":false,\n"
          << "  \"timing_source\":\"wicked_dx12_gpu_timestamp\",\n"
          << "  \"semantic_labels_used_by_controller\":false,\n"
          << "  \"target_frame_ms\":" << target_ms_ << ",\n"
          << "  \"target_calibration\":\"global_baseline_p40\",\n"
          << "  \"baseline\":{\"samples\":" << baseline_.frames.size()
          << ",\"p50_ms\":" << bp50 << ",\"p99_ms\":" << bp99
          << ",\"miss_ratio\":" << bmiss
          << ",\"mean_overshoot_ms\":" << MeanOvershoot(baseline_) << "},\n"
          << "  \"adaptive\":{\"samples\":" << adaptive_.frames.size()
          << ",\"p50_ms\":" << ap50 << ",\"p99_ms\":" << ap99
          << ",\"miss_ratio\":" << amiss
          << ",\"mean_overshoot_ms\":" << MeanOvershoot(adaptive_) << "},\n"
          << "  \"scene_pairs\":" << scene_pairs << ",\n"
          << "  \"scene_wins\":" << scene_wins << ",\n"
          << "  \"scenes\":[\n";

        for (std::size_t i = 0; i < kScenes.size(); ++i)
        {
            const auto& b = baseline_scenes_[i];
            const auto& a = adaptive_scenes_[i];
            f << "    {\"truth_label\":\"" << kScenes[i].name
              << "\",\"baseline_samples\":" << b.frames.size()
              << ",\"adaptive_samples\":" << a.frames.size()
              << ",\"baseline_p50_ms\":" << Percentile(b.frames, 0.50)
              << ",\"adaptive_p50_ms\":" << Percentile(a.frames, 0.50)
              << ",\"baseline_miss_ratio\":" << MissRatio(b)
              << ",\"adaptive_miss_ratio\":" << MissRatio(a) << "}"
              << (i + 1 == kScenes.size() ? "\n" : ",\n");
        }

        f << "  ],\n"
          << "  \"host\":{\"resources\":" << hm.resources_observed
          << ",\"resources_destroyed\":" << hm.resources_destroyed
          << ",\"descriptor_writes\":" << hm.descriptor_writes
          << ",\"resource_uses\":" << hm.resource_uses
          << ",\"draws\":" << hm.draws_observed
          << ",\"indexed_draws\":" << hm.indexed_draws_observed
          << ",\"dispatches\":" << hm.dispatches_observed
          << ",\"indirect\":" << hm.indirect_observed
          << ",\"draw_items\":" << hm.draw_items_observed
          << ",\"dispatch_groups\":" << hm.dispatch_groups_observed
          << ",\"barriers\":" << hm.barriers_observed
          << ",\"copies\":" << hm.copies_observed
          << ",\"queue_submits\":" << hm.queue_submits
          << ",\"fence_signals\":" << hm.fence_signals
          << ",\"completion_updates\":" << hm.completion_updates
          << ",\"presents\":" << hm.presents
          << ",\"events_drained\":" << hm.events_drained
          << ",\"failed_observations\":" << hm.failed_observations
          << ",\"bridge_rejections\":" << hm.bridge_rejections << "},\n"
          << "  \"graph\":{\"resources\":" << host_->graph().resource_count()
          << ",\"errors\":" << host_->graph().errors() << "},\n";
        WriteSemanticAudit(f);
        f << "  \"stage15_semantics\":{\"observer_only\":true"
          << ",\"confidence_basis\":\"heuristic_score\",\"accuracy_validated\":false"
          << ",\"capture_phase\":\"adaptive_end_before_recovery\",\"capture_frame\":" << semantic_snapshot_frame_
          << ",\"alive_resources\":" << semantic_predictions.size()
          << ",\"known_resources\":" << semantic_known
          << ",\"coverage\":" << semantic_coverage
          << ",\"high_confidence_ratio\":" << semantic_high_confidence_ratio
          << ",\"mean_confidence\":" << semantic_mean_confidence
          << ",\"counts\":[";
        for (std::size_t i = 0; i < semantic_counts.size(); ++i)
        {
            if (i) f << ",";
            f << semantic_counts[i];
        }
        f << "]},\n"
          << "  \"scene_understanding\":{\"observer_only\":true"
          << ",\"truth_labels_used_for_inference\":false"
          << ",\"same_scene_threshold\":" << scene_understanding_.config().same_scene_distance
          << ",\"cluster_threshold\":" << scene_understanding_.config().cluster_distance
          << ",\"cluster_count\":" << scene_clusters_.cluster_count()
          << ",\"baseline\":[";
        for (std::size_t i = 0; i < baseline_scene_semantics_.size(); ++i)
        {
            if (i) f << ",";
            WriteSceneSemanticCapture(f, baseline_scene_semantics_[i]);
        }
        f << "],\"adaptive\":[";
        for (std::size_t i = 0; i < adaptive_scene_semantics_.size(); ++i)
        {
            if (i) f << ",";
            WriteSceneSemanticCapture(f, adaptive_scene_semantics_[i]);
        }
        f << "],\"baseline_to_adaptive_distance\":[";
        for (std::size_t i = 0; i < baseline_scene_semantics_.size(); ++i)
        {
            if (i) f << ",";
            f << "[";
            for (std::size_t j = 0; j < adaptive_scene_semantics_.size(); ++j)
            {
                if (j) f << ",";
                double distance = 1.0;
                if (baseline_scene_semantics_[i].captured && adaptive_scene_semantics_[j].captured)
                {
                    distance = scene_understanding_.compare(
                        baseline_scene_semantics_[i].signature,
                        adaptive_scene_semantics_[j].signature).distance;
                }
                f << distance;
            }
            f << "]";
        }
        f << "]},\n"
          << "  \"governor\":{\"quality_actions_executed\":" << gm.quality_actions_executed
          << ",\"learned_effects\":" << learned
          << ",\"quality_domains_executed\":" << domains
          << ",\"adaptive_ticks\":" << adaptive_ticks_
          << ",\"adaptive_quality_actions\":" << adaptive_quality_actions_
          << ",\"adaptive_direction_changes\":" << adaptive_direction_changes_
          << ",\"adaptive_action_rate\":" << action_rate
          << ",\"adaptive_direction_change_rate\":" << direction_rate
          << ",\"restore_probes\":" << adaptive_restore_probes_
          << ",\"restore_backoffs\":" << adaptive_restore_backoffs_
          << ",\"recovery_ticks\":" << recovery_ticks_
          << ",\"quality_backend_failures\":" << gm.quality_backend_failures << "},\n"
          << "  \"hook_timings\":";
        WriteHookTimings(f);
        f << ",\n"
          << "  \"diagnostics\":{\"cpu_frame_p50_ms\":" << cpu_frame_p50
          << ",\"cpu_frame_p99_ms\":" << cpu_frame_p99
          << ",\"cpu_frame_max_ms\":" << cpu_frame_max
          << ",\"cpu_hitches_50ms\":" << cpu_hitches_50ms
          << ",\"cpu_hitches_100ms\":" << cpu_hitches_100ms
          << ",\"arc_present_p50_ms\":" << arc_present_p50
          << ",\"arc_present_p99_ms\":" << arc_present_p99
          << ",\"arc_present_max_ms\":" << arc_present_max
          << ",\"arc_tick_p50_ms\":" << arc_tick_p50
          << ",\"arc_tick_p99_ms\":" << arc_tick_p99
          << ",\"arc_tick_max_ms\":" << arc_tick_max
          << ",\"present_failures\":" << present_failures_
          << ",\"last_present_hresult\":" << static_cast<std::int64_t>(last_present_failure_)
          << ",\"device_removed_reason\":" << static_cast<std::int64_t>(device_removed_reason_) << "},\n"
          << "  \"performance_win\":" << (performance ? "true" : "false") << ",\n"
          << "  \"bounded_churn\":" << (bounded_churn ? "true" : "false") << ",\n"
          << "  \"final_quality_full\":" << (full_recovery ? "true" : "false") << "\n"
          << "}\n";
    }

    void WriteSemanticAudit(std::ostream& out) {
        std::scoped_lock lock(truth_mutex_);
        std::array<std::uint64_t,6> populations{},correct_by_family{};
        std::array<std::array<std::uint64_t,arc::kInferredResourceSemanticCount>,6> confusion{};
        std::uint64_t covered=0,correct=0,high=0,high_wrong=0;
        for(const auto& [id,sample]:truth_samples_) {
            (void)id;
            auto expected=static_cast<unsigned>(arc_wicked::audit::catalog[sample.entry].family);
            ++populations[expected];
            ++confusion[expected][static_cast<unsigned>(sample.predicted)];
            const bool match=arc_wicked::audit::predicted_family(sample.predicted)==int(expected);
            if(sample.predicted!=arc::InferredResourceSemantic::Unknown) ++covered;
            if(match) { ++correct; ++correct_by_family[expected]; }
            if(sample.confidence>=.75f) { ++high; if(!match) ++high_wrong; }
        }
        const auto families=std::count_if(populations.begin(),populations.end(),[](auto n){return n>0;});
        out << "  \"resource_semantic_audit\":{\"schema\":1,\"catalog_version\":1,\"scope\":\"six_resource_use_families\","
            << "\"labels_used_for_inference\":false,\"fine_subtypes_validated\":false,\"truncated\":" << (truth_truncated_?"true":"false")
            << ",\"samples\":" << truth_samples_.size() << ",\"covered\":" << covered << ",\"correct\":" << correct
            << ",\"families\":" << families << ",\"coverage\":" << (truth_samples_.empty()?0.0:double(covered)/truth_samples_.size())
            << ",\"family_precision\":" << (covered?double(correct)/covered:0.0)
            << ",\"family_recall\":" << (truth_samples_.empty()?0.0:double(correct)/truth_samples_.size())
            << ",\"high_confidence_samples\":" << high << ",\"high_confidence_wrong\":" << high_wrong << ",\"confusion\":[";
        for(unsigned i=0;i<6;++i) { if(i)out<<',';out<<'[';for(unsigned j=0;j<arc::kInferredResourceSemanticCount;++j){if(j)out<<',';out<<confusion[i][j];}out<<']'; }
        out << "],\"observations\":[";
        bool first=true;
        for(const auto& [id,sample]:truth_samples_) {
            if(!first)out<<',';first=false;
            const auto& entry=arc_wicked::audit::catalog[sample.entry];
            out << "{\"resource\":" << id << ",\"name\":\"" << entry.name << "\",\"expected_family\":" << int(entry.family)
                << ",\"predicted_class\":" << int(sample.predicted) << ",\"confidence\":" << sample.confidence << '}';
        }
        out << "]},\n";
    }

    struct AuditSample { int entry; arc::InferredResourceSemantic predicted; float confidence; };
    std::mutex truth_mutex_;
    std::unordered_map<ID3D12Resource*,std::pair<arc::ResourceId,int>> truth_live_;
    std::unordered_map<arc::ResourceId,AuditSample> truth_samples_;
    bool truth_truncated_=false;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    ID3D12DescriptorHeap* resource_heap_ = nullptr;
    ID3D12DescriptorHeap* sampler_heap_ = nullptr;
    std::unique_ptr<arc::dx12::NativeHostAdapter> host_;

    std::mutex pending_mutex_;
    std::unordered_map<ID3D12CommandList*, CommandPending> pending_;
    std::mutex queue_mutex_;
    std::unordered_map<ID3D12CommandQueue*, QueueState> queues_;

    std::string experiment_mode_;
    std::size_t scene_offset_ = 0;
    std::array<Stats, kScenes.size()> experiment_cpu_{};
    std::uint64_t invalid_samples_ = 0;
    std::ofstream cpu_profile_;
    std::chrono::steady_clock::time_point cpu_profile_start_{};
    int cpu_profile_scene_ = 1, cpu_profile_seconds_ = 8;
    std::uint64_t cpu_profile_frame_ = 0, cpu_profile_rows_ = 0;
    bool cpu_profile_selected_ = false;
    std::vector<arc::ResourceSemanticPrediction> measured_semantics_;
    arc::FrameId semantic_snapshot_frame_ = 0;
    Phase phase_ = Phase::Dormant;
    std::chrono::steady_clock::time_point phase_start_{};
    std::chrono::steady_clock::time_point settle_until_{};
    std::size_t current_scene_ = 0;

    int seconds_ = 30;
    int warmup_seconds_ = 5;
    int recovery_seconds_ = 20;
    int settle_milliseconds_ = 1500;
    int control_frames_ = 16;

    std::filesystem::path output_path_ = "stage14_5-wicked.json";
    std::string arc_source_sha_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;

    Stats warmup_;
    Stats baseline_;
    Stats adaptive_;
    std::array<Stats, kScenes.size()> baseline_scenes_{};
    std::array<Stats, kScenes.size()> adaptive_scenes_{};
    std::vector<double> control_window_;
    std::vector<double> cpu_frame_intervals_ms_;
    std::vector<double> arc_present_cost_ms_;
    std::vector<double> arc_tick_cost_ms_;
    std::chrono::steady_clock::time_point last_harness_update_{};
    bool have_last_harness_update_ = false;
    std::uint64_t present_failures_ = 0;
    HRESULT last_present_failure_ = S_OK;
    HRESULT device_removed_reason_ = S_OK;

    arc::SceneUnderstandingInferencer scene_understanding_{};
    arc::SceneSemanticClusterer scene_clusters_{};
    arc::SceneObservationCheckpoint scene_checkpoint_{};
    bool scene_semantic_window_started_ = false;
    std::array<SceneSemanticCapture, kScenes.size()> baseline_scene_semantics_{};
    std::array<SceneSemanticCapture, kScenes.size()> adaptive_scene_semantics_{};

    double target_ms_ = 0.0;
    double recovery_target_ms_ = 0.0;
    bool profiles_registered_ = false;

    std::size_t shadow_level_ = 0;
    std::size_t geometry_level_ = 0;
    std::size_t bandwidth_level_ = 0;
    std::array<bool, 3> quality_domains_{};

    arc::dx12::NativeHostAdapterMetrics last_control_metrics_{};
    std::uint64_t adaptive_ticks_ = 0;
    std::uint64_t recovery_ticks_ = 0;
    std::uint64_t adaptive_quality_actions_ = 0;
    std::uint64_t adaptive_direction_changes_ = 0;
    std::uint64_t adaptive_restore_probes_ = 0;
    std::uint64_t adaptive_restore_backoffs_ = 0;
};

Bridge* g_bridge = nullptr;
std::mutex g_bridge_mutex;

void ArcBreadcrumb(std::uint32_t hook) noexcept
{
    g_arc_last_hook.store(hook, std::memory_order_relaxed);
}

LONG WINAPI ArcUnhandledException(EXCEPTION_POINTERS* info) noexcept
{
    const auto path = EnvString(L"ARC_WICKED_CRASH_OUTPUT");
    if (!path.empty())
    {
        HANDLE file = CreateFileW(
            path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            char buffer[512]{};
            const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
            const void* address = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
            const int n = sprintf_s(
                buffer, sizeof(buffer),
                "exception=0x%08lX\r\naddress=%p\r\nlast_hook=%u\r\nscene_index=%u\r\nphase=%u\r\n",
                static_cast<unsigned long>(code),
                address,
                g_arc_last_hook.load(std::memory_order_relaxed),
                g_arc_last_scene.load(std::memory_order_relaxed),
                g_arc_last_phase.load(std::memory_order_relaxed));
            if (n > 0)
            {
                DWORD written = 0;
                WriteFile(file, buffer, static_cast<DWORD>(n), &written, nullptr);
            }
            CloseHandle(file);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

Bridge* GetBridge() noexcept
{
    return g_bridge;
}

} // namespace

extern "C" void ARCWickedDeviceReady(
    ID3D12Device* device,
    ID3D12DescriptorHeap* resource_heap,
    ID3D12DescriptorHeap* sampler_heap) noexcept
{
    if (!device) return;
    std::scoped_lock lock(g_bridge_mutex);
    if (!g_bridge)
    {
        g_hooks_enabled = EnvString(L"ARC_WICKED_EXPERIMENT_MODE") != L"off";
        g_hook_timing_enabled = EnvInt(L"ARC_WICKED_HOOK_TIMING", 1, 0, 1) != 0;
        SetUnhandledExceptionFilter(ArcUnhandledException);
        g_bridge = new Bridge(device, resource_heap, sampler_heap);
    }
}

extern "C" void ARCWickedCommandDestroyed(ID3D12CommandList* command) noexcept { if(auto* b=GetBridge()) b->CommandDestroyed(command); }
extern "C" void ARCWickedQueueDestroyed(ID3D12CommandQueue* queue) noexcept { if(auto* b=GetBridge()) b->QueueDestroyed(queue); }
extern "C" void ARCWickedResourceTruth(ID3D12Resource* resource, const char* name) noexcept { if(auto* b=GetBridge()) b->ResourceTruth(resource,name); }
extern "C" void ARCWickedDeviceDestroyed() noexcept {
    // Device owner has stopped recording and waited for GPU completion.
    // No cross-TU global mutex here: this also runs during static shutdown.
    auto* retired=g_bridge; g_bridge=nullptr; delete retired;
}

extern "C" void ARCWickedResourceCreated(ID3D12Resource* resource) noexcept
{
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(10);
    ArcBreadcrumb(10);
    if (auto* b = GetBridge()) b->ResourceCreated(resource);
}

extern "C" void ARCWickedResourceDestroyed(ID3D12Resource* resource) noexcept
{
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(11);
    ArcBreadcrumb(11);
    if (auto* b = GetBridge()) b->ResourceDestroyed(resource);
}

extern "C" void ARCWickedObserveSRV(
    ID3D12DescriptorHeap* heap,
    std::uint32_t index,
    ID3D12Resource* resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* view) noexcept
{
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(20);
    ArcBreadcrumb(20);
    if (auto* b = GetBridge()) b->ObserveSRV(heap, index, resource, view);
}

extern "C" void ARCWickedObserveUAV(
    ID3D12DescriptorHeap* heap,
    std::uint32_t index,
    ID3D12Resource* resource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* view) noexcept
{
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(21);
    ArcBreadcrumb(21);
    if (auto* b = GetBridge()) b->ObserveUAV(heap, index, resource, view);
}

extern "C" void ARCWickedObserveSampler(
    ID3D12DescriptorHeap* heap,
    std::uint32_t index) noexcept
{
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(22);
    ArcBreadcrumb(22);
    if (auto* b = GetBridge()) b->ObserveSampler(heap, index);
}

extern "C" void ARCWickedCommandBegin(
    ID3D12CommandList* command,
    D3D12_COMMAND_LIST_TYPE type) noexcept
{
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(30);
    ArcBreadcrumb(30);
    if (auto* b = GetBridge()) b->CommandBegin(command, type);
}

extern "C" void ARCWickedResourceUse(
    ID3D12CommandList* command,
    ID3D12Resource* resource,
    bool write) noexcept
{
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(31);
    ArcBreadcrumb(31);
    if (auto* b = GetBridge()) b->ResourceUse(command, resource, write);
}

extern "C" void ARCWickedTransition(
    ID3D12CommandList* command,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before_state,
    D3D12_RESOURCE_STATES after_state,
    std::uint32_t subresource) noexcept
{
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(32);
    ArcBreadcrumb(32);
    if (auto* b = GetBridge())
        b->Transition(command, resource, before_state, after_state, subresource);
}

extern "C" void ARCWickedCopy(
    ID3D12CommandList* command,
    ID3D12Resource* source,
    ID3D12Resource* destination,
    std::uint64_t approximate_bytes,
    std::uint32_t kind) noexcept
{
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(33);
    ArcBreadcrumb(33);
    if (auto* b = GetBridge()) b->Copy(command, source, destination, approximate_bytes, kind);
}

extern "C" void ARCWickedCountCommand(
    ID3D12CommandList* command,
    std::uint32_t kind,
    std::uint64_t work_items) noexcept
{
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(34);
    ArcBreadcrumb(34);
    if (auto* b = GetBridge()) b->Count(command, kind, work_items);
}

extern "C" void ARCWickedSubmit(
    ID3D12CommandQueue* queue,
    ID3D12CommandList* const* commands,
    std::size_t count,
    D3D12_COMMAND_LIST_TYPE type) noexcept
{
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(40);
    ArcBreadcrumb(40);
    if (auto* b = GetBridge()) b->Submit(queue, commands, count, type);
}

extern "C" void ARCWickedPresent(
    std::uint64_t swapchain_id,
    std::uint32_t sync_interval,
    std::uint32_t flags,
    HRESULT result) noexcept
{
    if (FAILED(result)) g_present_failures.fetch_add(1, std::memory_order_relaxed);
    if (!g_hooks_enabled) return;
    OptionalHookTimer hook_timer(50);
    ArcBreadcrumb(50);
    if (auto* b = GetBridge()) b->Present(swapchain_id, sync_interval, flags, result);
}

extern "C" void ARCWickedCpuSample(const char* name, double milliseconds) noexcept
{
    if (auto* b = GetBridge()) b->CpuSample(name, milliseconds);
}

namespace arc_wicked {

void PollCpuProfile() noexcept
{
    if (auto* b = GetBridge()) b->PollCpuProfile();
}

void HarnessUpdate(
    wi::gui::ComboBox& test_selector,
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    ArcBreadcrumb(60);
    if (auto* b = GetBridge()) b->HarnessUpdate(test_selector, width, height);
}

void ForceFullQuality() noexcept
{
    if (auto* b = GetBridge()) b->ForceFullQuality();
}

bool Finished() noexcept
{
    if (auto* b = GetBridge()) return b->Finished();
    return false;
}

} // namespace arc_wicked
