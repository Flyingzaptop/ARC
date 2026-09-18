#include "stdafx.h"
#include "ArcWickedBridge.h"
#include "ArcWickedHooks.h"

#include "arc/dx12_host_adapter.hpp"
#include "arc/quality_profile.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
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
};

struct CommandPending
{
    std::unordered_map<ID3D12Resource*, bool> uses;
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

        seconds_ = EnvInt(L"ARC_WICKED_SECONDS", 30, 10, 120);
        warmup_seconds_ = EnvInt(L"ARC_WICKED_WARMUP_SECONDS", 5, 2, 20);
        recovery_seconds_ = EnvInt(L"ARC_WICKED_RECOVERY_SECONDS", 20, 5, 60);
        settle_milliseconds_ = EnvInt(L"ARC_WICKED_SCENE_SETTLE_MS", 1500, 500, 5000);
        control_frames_ = EnvInt(L"ARC_WICKED_CONTROL_FRAMES", 16, 4, 120);
        arc_source_sha_ = Narrow(EnvString(L"ARC_SOURCE_SHA"));
        const auto output = EnvString(L"ARC_WICKED_OUTPUT");
        if (!output.empty()) output_path_ = output;
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

    void ResourceUse(ID3D12CommandList* command, ID3D12Resource* resource, bool write) noexcept
    {
        if (!host_ || !command || !resource) return;
        if (!host_->resource_id(resource)) (void)host_->observe_external_resource(resource);
        std::scoped_lock lock(pending_mutex_);
        auto& current = pending_[command].uses[resource];
        current = current || write;
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

    void Count(ID3D12CommandList* command, std::uint32_t kind) noexcept
    {
        if (!command) return;
        std::scoped_lock lock(pending_mutex_);
        auto& counters = pending_[command].counters;
        switch (kind)
        {
        case 0: ++counters.draws; break;
        case 1: ++counters.indexed_draws; break;
        case 2: ++counters.dispatches; break;
        default: ++counters.indirect; break;
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

            for (const auto& [resource, write] : state.uses)
                (void)host_->observe_resource_use(command, resource, write);

            const auto& c = state.counters;
            if (c.draws || c.indexed_draws || c.dispatches || c.indirect)
                (void)host_->observe_command_counters(
                    command, c.draws, c.indexed_draws, c.dispatches, c.indirect);

            (void)host_->observe_command_list_closed(command);
        }

        (void)host_->observe_execute_command_lists(
            queue, std::span<ID3D12CommandList* const>(commands, count));

        QueueState* queue_state = nullptr;
        {
            std::scoped_lock lock(queue_mutex_);
            auto [it, inserted] = queues_.try_emplace(queue);
            queue_state = &it->second;
            queue_state->type = type;
            if (inserted && device_)
            {
                if (SUCCEEDED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue_state->fence))))
                    (void)host_->bind_completion_fence(queue, queue_state->fence.Get());
            }
        }

        if (queue_state && queue_state->fence)
        {
            const std::uint64_t value = ++queue_state->value;
            if (SUCCEEDED(queue->Signal(queue_state->fence.Get(), value)))
            {
                (void)host_->observe_fence_signal(queue, queue_state->fence.Get(), value);
                (void)host_->poll_completion(queue);
            }
        }
    }

    void Present(
        std::uint64_t swapchain_id,
        std::uint32_t sync_interval,
        std::uint32_t flags,
        HRESULT result) noexcept
    {
        if (!host_) return;
        (void)host_->observe_present(swapchain_id, sync_interval, flags, result);
        (void)host_->poll_all_completions();
        (void)host_->drain();
    }

    void HarnessUpdate(wi::gui::ComboBox& selector, std::uint32_t width, std::uint32_t height) noexcept
    {
        if (!host_ || phase_ == Phase::Done) return;

        width_ = width;
        height_ = height;
        wi::eventhandler::SetVSync(false);
        wi::renderer::SetTemporalAAEnabled(false);
        wi::profiler::SetEnabled(true);

        const auto now = std::chrono::steady_clock::now();
        if (phase_ == Phase::Dormant)
        {
            phase_ = Phase::Warmup;
            phase_start_ = now;
            current_scene_ = 1;
            selector.SetSelected(kScenes[current_scene_].combo_index);
            AfterSceneSwitch(now);
            return;
        }

        const double gpu_ms = static_cast<double>(wi::profiler::GetLastGPUFrameTimeMS());
        const double elapsed = std::chrono::duration<double>(now - phase_start_).count();

        if (phase_ == Phase::Warmup)
        {
            if (gpu_ms > 0.0 && now >= settle_until_) warmup_.frames.push_back(gpu_ms);
            if (elapsed >= static_cast<double>(warmup_seconds_))
            {
                EnterMeasuredPhase(Phase::Baseline, selector, now);
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
                if (phase_ == Phase::Baseline) FinishBaseline(selector, now);
                else FinishAdaptive(selector, now);
                return;
            }

            if (desired != current_scene_)
            {
                current_scene_ = desired;
                selector.SetSelected(kScenes[current_scene_].combo_index);
                AfterSceneSwitch(now);
                return;
            }

            const double within_slot = elapsed - static_cast<double>(current_scene_) * slot_s;
            if (within_slot < settle_s || gpu_ms <= 0.0) return;

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

    void ForceFullQuality() noexcept
    {
        shadow_level_ = 0;
        geometry_level_ = 0;
        bandwidth_level_ = 0;
        wi::renderer::SetShadowProps2D(2048);
        wi::renderer::SetShadowPropsCube(1024);
        wi::renderer::SetTessellationEnabled(true);
        wi::renderer::SetDisableAlbedoMaps(false);
        wi::renderer::SetTemporalAAEnabled(false);
    }

private:
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

    void AfterSceneSwitch(std::chrono::steady_clock::time_point now)
    {
        settle_until_ = now + std::chrono::milliseconds(settle_milliseconds_);
        wi::eventhandler::SetVSync(false);
        wi::renderer::SetTemporalAAEnabled(false);
        wi::profiler::SetEnabled(true);
        control_window_.clear();
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
        bandwidth.label = "wicked albedo sampling";
        bandwidth.semantic = arc::QualitySemanticClass::Environment;
        bandwidth.importance = {0.65, 0.95, 0.40, 0.45, 0.60};
        bandwidth.confidence = 0.72;
        bandwidth.reversible = true;
        bandwidth.levels = {
            {0.40, baseline_p50 * 0.12, 0},
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
        frame.memory_bandwidth_fraction = std::max(use_pressure, descriptor_pressure);
        frame.geometry_pressure = draw_pressure;
        frame.lighting_pressure = std::max(dispatch_pressure, draw_pressure * 0.72);
        frame.shadow_pressure = std::max(barrier_pressure, draw_pressure * 0.82);
        frame.raster_pressure = std::clamp(draw_pressure * 0.55 + use_pressure * 0.45, 0.0, 1.0);

        control_window_.clear();
        (void)host_->frame_tick(frame, false);
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
            if (!restore)
            {
                if (action.sequence != 0 || bandwidth_level_ != 0)
                    return arc::RuntimeBackendStatus::Failure;
                bandwidth_level_ = 1;
                wi::renderer::SetDisableAlbedoMaps(true);
            }
            else
            {
                if (bandwidth_level_ != 1)
                    return arc::RuntimeBackendStatus::Failure;
                bandwidth_level_ = 0;
                wi::renderer::SetDisableAlbedoMaps(false);
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
                probe.levels = {{0.5, 0.1, 0}};
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
            action_rate <= 0.010 &&
            direction_rate <= 0.005;
        const bool full_recovery = FullQuality() && qs.active_actions == 0;
        const bool native_1080 = width_ == 1920 && height_ == 1080;

        const bool valid =
            host_clean && timing_valid && complex_renderer && scene_coverage &&
            physical_quality && learning && performance && bounded_churn &&
            full_recovery && native_1080 && profiles_registered_;

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
          << ",\"errors\":" << host_->graph().errors() << "},\n"
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
          << "  \"performance_win\":" << (performance ? "true" : "false") << ",\n"
          << "  \"bounded_churn\":" << (bounded_churn ? "true" : "false") << ",\n"
          << "  \"final_quality_full\":" << (full_recovery ? "true" : "false") << "\n"
          << "}\n";
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    ID3D12DescriptorHeap* resource_heap_ = nullptr;
    ID3D12DescriptorHeap* sampler_heap_ = nullptr;
    std::unique_ptr<arc::dx12::NativeHostAdapter> host_;

    std::mutex pending_mutex_;
    std::unordered_map<ID3D12CommandList*, CommandPending> pending_;
    std::mutex queue_mutex_;
    std::unordered_map<ID3D12CommandQueue*, QueueState> queues_;

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

std::unique_ptr<Bridge> g_bridge;
std::mutex g_bridge_mutex;

Bridge* GetBridge() noexcept
{
    return g_bridge.get();
}

} // namespace

extern "C" void ARCWickedDeviceReady(
    ID3D12Device* device,
    ID3D12DescriptorHeap* resource_heap,
    ID3D12DescriptorHeap* sampler_heap) noexcept
{
    if (!device) return;
    std::scoped_lock lock(g_bridge_mutex);
    if (!g_bridge) g_bridge = std::make_unique<Bridge>(device, resource_heap, sampler_heap);
}

extern "C" void ARCWickedResourceCreated(ID3D12Resource* resource) noexcept
{
    if (auto* b = GetBridge()) b->ResourceCreated(resource);
}

extern "C" void ARCWickedResourceDestroyed(ID3D12Resource* resource) noexcept
{
    if (auto* b = GetBridge()) b->ResourceDestroyed(resource);
}

extern "C" void ARCWickedObserveSRV(
    ID3D12DescriptorHeap* heap,
    std::uint32_t index,
    ID3D12Resource* resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* view) noexcept
{
    if (auto* b = GetBridge()) b->ObserveSRV(heap, index, resource, view);
}

extern "C" void ARCWickedObserveUAV(
    ID3D12DescriptorHeap* heap,
    std::uint32_t index,
    ID3D12Resource* resource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* view) noexcept
{
    if (auto* b = GetBridge()) b->ObserveUAV(heap, index, resource, view);
}

extern "C" void ARCWickedObserveSampler(
    ID3D12DescriptorHeap* heap,
    std::uint32_t index) noexcept
{
    if (auto* b = GetBridge()) b->ObserveSampler(heap, index);
}

extern "C" void ARCWickedCommandBegin(
    ID3D12CommandList* command,
    D3D12_COMMAND_LIST_TYPE type) noexcept
{
    if (auto* b = GetBridge()) b->CommandBegin(command, type);
}

extern "C" void ARCWickedResourceUse(
    ID3D12CommandList* command,
    ID3D12Resource* resource,
    bool write) noexcept
{
    if (auto* b = GetBridge()) b->ResourceUse(command, resource, write);
}

extern "C" void ARCWickedTransition(
    ID3D12CommandList* command,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before_state,
    D3D12_RESOURCE_STATES after_state,
    std::uint32_t subresource) noexcept
{
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
    if (auto* b = GetBridge()) b->Copy(command, source, destination, approximate_bytes, kind);
}

extern "C" void ARCWickedCountCommand(
    ID3D12CommandList* command,
    std::uint32_t kind) noexcept
{
    if (auto* b = GetBridge()) b->Count(command, kind);
}

extern "C" void ARCWickedSubmit(
    ID3D12CommandQueue* queue,
    ID3D12CommandList* const* commands,
    std::size_t count,
    D3D12_COMMAND_LIST_TYPE type) noexcept
{
    if (auto* b = GetBridge()) b->Submit(queue, commands, count, type);
}

extern "C" void ARCWickedPresent(
    std::uint64_t swapchain_id,
    std::uint32_t sync_interval,
    std::uint32_t flags,
    HRESULT result) noexcept
{
    if (auto* b = GetBridge()) b->Present(swapchain_id, sync_interval, flags, result);
}

namespace arc_wicked {

void HarnessUpdate(
    wi::gui::ComboBox& test_selector,
    std::uint32_t width,
    std::uint32_t height) noexcept
{
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
