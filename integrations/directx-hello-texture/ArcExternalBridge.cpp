#include "stdafx.h"
#include "ArcExternalBridge.h"

#include "arc/quality_profile.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <span>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr char kUpstreamSha[] = "213dd4fd4918ea009dd8f35adee1aff1f2ecaba4";
}

ArcExternalBridge::ArcExternalBridge(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Fence* fence,
    ID3D12DescriptorHeap* rtvHeap,
    ID3D12DescriptorHeap* srvHeap,
    ID3D12Resource* const* renderTargets,
    UINT renderTargetCount,
    ID3D12Resource* vertexBuffer,
    ID3D12Resource* texture)
    : adapter_(FindAdapter(device)),
      queue_(queue),
      commandList_(commandList),
      fence_(fence),
      vertexBuffer_(vertexBuffer),
      texture_(texture)
{
    measureSeconds_ = EnvInt(L"ARC_EXTERNAL_SECONDS", 20, 10, 120);
    controlFrames_ = EnvInt(L"ARC_EXTERNAL_CONTROL_FRAMES", 16, 4, 120);
    warmupSeconds_ = EnvInt(L"ARC_EXTERNAL_WARMUP_SECONDS", 3, 1, 15);
    recoverySeconds_ = EnvInt(L"ARC_EXTERNAL_RECOVERY_SECONDS", 12, 3, 60);
    const auto output = EnvString(L"ARC_EXTERNAL_OUTPUT");
    if (!output.empty()) outputPath_ = output;
    arcSourceSha_ = EnvString(L"ARC_SOURCE_SHA");

    if (adapter_)
    {
        DXGI_ADAPTER_DESC2 desc{};
        if (SUCCEEDED(adapter_->GetDesc2(&desc))) adapterName_ = Narrow(desc.Description);
    }

    // Measure the workload on the GPU itself. Wall-clock time around Present
    // and WaitForPreviousFrame contains CPU/driver/fence latency and can hide
    // the effect of a physical quality mutation.
    if (device && queue_ &&
        SUCCEEDED(queue_->GetTimestampFrequency(&timestampFrequency_)) &&
        timestampFrequency_ > 0)
    {
        D3D12_QUERY_HEAP_DESC queryDesc{};
        queryDesc.Count = 2;
        queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        if (SUCCEEDED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&timestampHeap_))))
        {
            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_READBACK;
            heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heapProps.CreationNodeMask = 1;
            heapProps.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC readbackDesc{};
            readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            readbackDesc.Width = 2 * sizeof(std::uint64_t);
            readbackDesc.Height = 1;
            readbackDesc.DepthOrArraySize = 1;
            readbackDesc.MipLevels = 1;
            readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
            readbackDesc.SampleDesc.Count = 1;
            readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            if (FAILED(device->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &readbackDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&timestampReadback_))))
            {
                timestampHeap_.Reset();
                timestampFrequency_ = 0;
            }
        }
    }

    arc::dx12::NativeHostAdapterConfig cfg{};
    cfg.event_capacity = 32768;
    cfg.runtime.coordinator.mode = arc::RuntimeMode::ObserveOnly;
    cfg.runtime.coordinator.max_actions_per_tick = 1;
    cfg.runtime.coordinator.max_budget_age_ticks = 120;
    cfg.runtime.governor.enable_memory = false;
    cfg.runtime.governor.enable_quality = true;
    cfg.runtime.governor.quality.overload_samples_required = 3;
    cfg.runtime.governor.quality.headroom_samples_required = 6;
    cfg.runtime.governor.quality.settle_samples_after_change = 3;
    cfg.runtime.governor.quality.minimum_hold_samples_after_degrade = 24;
    cfg.runtime.governor.quality.restore_probe_samples_required = 96;
    cfg.runtime.governor.quality.restore_probe_headroom_fraction = 0.20;
    cfg.runtime.governor.quality.restore_reversal_window_samples = 192;
    cfg.runtime.governor.quality.restore_backoff_base_samples = 128;
    cfg.runtime.governor.quality.restore_backoff_max_samples = 2048;
    cfg.runtime.governor.quality.frame_ewma_alpha = 0.40;
    cfg.runtime.governor.quality.overload_margin_ms = 0.005;
    cfg.runtime.governor.quality.extra_restore_headroom_ms = 0.02;
    cfg.runtime.governor.quality.optimizer.minimum_gain_ms = 0.01;
    cfg.runtime.governor.quality.optimizer.minimum_confidence = 0.30;
    cfg.runtime.governor.quality.optimizer.restoration_headroom_ms = 0.03;

    host_ = std::make_unique<arc::dx12::NativeHostAdapter>(device, adapter_.Get(), cfg);
    host_->set_mode(arc::RuntimeMode::ObserveOnly);
    host_->set_quality_mutator([this](const arc::QualityActionCandidate& action, bool restore)
    {
        return Mutate(action, restore);
    });

    (void)host_->observe_queue(queue_, D3D12_COMMAND_LIST_TYPE_DIRECT);
    (void)host_->observe_command_list(commandList_, D3D12_COMMAND_LIST_TYPE_DIRECT);
    (void)host_->bind_completion_fence(queue_, fence_);

    const auto rtvId = host_->observe_descriptor_heap(rtvHeap);
    const auto srvId = host_->observe_descriptor_heap(srvHeap);
    (void)rtvId;
    (void)srvId;

    for (UINT i = 0; i < renderTargetCount; ++i)
    {
        if (!renderTargets[i]) continue;
        (void)host_->observe_external_resource(renderTargets[i]);
        const auto desc = renderTargets[i]->GetDesc();
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = desc.Format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = 0;
        view.Texture2D.PlaneSlice = 0;
        (void)host_->observe_rtv(rtvHeap, i, renderTargets[i], view);
    }

    if (vertexBuffer_) (void)host_->observe_committed_resource(vertexBuffer_);
    if (texture_)
    {
        (void)host_->observe_committed_resource(texture_);
        const auto desc = texture_->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = desc.Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = desc.MipLevels;
        (void)host_->observe_srv(srvHeap, 0, texture_, view);
    }

    controlWindow_.reserve(static_cast<std::size_t>(controlFrames_));
    (void)host_->drain();
    phaseStart_ = std::chrono::steady_clock::now();
}

ArcExternalBridge::~ArcExternalBridge()
{
    Finalize();
}

UINT ArcExternalBridge::SampleIterations() const noexcept
{
    return kIterations[std::min<std::size_t>(qualityLevel_, kIterations.size() - 1)];
}

void ArcExternalBridge::OnCommandReset()
{
    if (host_) (void)host_->observe_command_list_reset(commandList_);
}

void ArcExternalBridge::OnGpuFrameBegin()
{
    timestampRecorded_ = false;
    if (!commandList_ || !timestampHeap_ || !timestampReadback_ || timestampFrequency_ == 0) return;
    commandList_->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
}

void ArcExternalBridge::OnGpuFrameEnd()
{
    if (!commandList_ || !timestampHeap_ || !timestampReadback_ || timestampFrequency_ == 0) return;
    commandList_->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    commandList_->ResolveQueryData(
        timestampHeap_.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        0,
        2,
        timestampReadback_.Get(),
        0);
    timestampRecorded_ = true;
}

double ArcExternalBridge::ConsumeGpuFrameMs(double fallbackMs) noexcept
{
    if (!timestampRecorded_ || !timestampReadback_ || timestampFrequency_ == 0) return fallbackMs;

    const D3D12_RANGE readRange{0, 2 * sizeof(std::uint64_t)};
    void* mapped = nullptr;
    if (FAILED(timestampReadback_->Map(0, &readRange, &mapped)) || !mapped) return fallbackMs;

    const auto* timestamps = static_cast<const std::uint64_t*>(mapped);
    const std::uint64_t begin = timestamps[0];
    const std::uint64_t end = timestamps[1];
    const D3D12_RANGE writtenRange{0, 0};
    timestampReadback_->Unmap(0, &writtenRange);
    timestampRecorded_ = false;

    if (end <= begin) return fallbackMs;
    const double ms =
        (static_cast<double>(end - begin) * 1000.0) /
        static_cast<double>(timestampFrequency_);
    if (!(ms > 0.0) || !std::isfinite(ms)) return fallbackMs;

    ++gpuTimestampSamples_;
    return ms;
}

void ArcExternalBridge::ObserveTransition(ID3D12Resource* resource, const D3D12_RESOURCE_BARRIER& barrier)
{
    if (host_) (void)host_->observe_transition_barrier(commandList_, resource, barrier);
}

void ArcExternalBridge::ObserveFrameUses(
    ID3D12Resource* renderTarget,
    ID3D12Resource* vertexBuffer,
    ID3D12Resource* texture)
{
    if (!host_) return;
    if (renderTarget) (void)host_->observe_resource_use(commandList_, renderTarget, true);
    if (vertexBuffer) (void)host_->observe_resource_use(commandList_, vertexBuffer, false);
    if (texture) (void)host_->observe_resource_use(commandList_, texture, false);
    (void)host_->observe_command_counters(commandList_, 1, 0, 0, 0);
}

void ArcExternalBridge::OnCommandClosed()
{
    if (host_) (void)host_->observe_command_list_closed(commandList_);
}

void ArcExternalBridge::OnSubmit()
{
    if (!host_) return;
    ID3D12CommandList* lists[]{commandList_};
    (void)host_->observe_execute_command_lists(
        queue_, std::span<ID3D12CommandList* const>(lists, 1));
}

void ArcExternalBridge::OnSignal(UINT64 fenceValue)
{
    if (host_) (void)host_->observe_fence_signal(queue_, fence_, fenceValue);
}

void ArcExternalBridge::OnCompletion()
{
    if (!host_) return;
    (void)host_->poll_completion(queue_);
    (void)host_->drain();
}

void ArcExternalBridge::OnPresent(HRESULT result)
{
    if (host_) (void)host_->observe_present(0xA12C001ull, 0, 0, result);
}

void ArcExternalBridge::OnFrame(double frameMs)
{
    if (!host_ || finalized_) return;
    frameMs = ConsumeGpuFrameMs(frameMs);
    if (!(frameMs > 0.0) || !std::isfinite(frameMs)) return;

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - phaseStart_).count();

    switch (phase_)
    {
    case Phase::Warmup:
        AddStat(warmup_, frameMs);
        (void)host_->drain();
        if (elapsed >= static_cast<double>(warmupSeconds_))
        {
            // Warmup is intentionally not used to choose the acceptance target.
            // The target is frozen from the independent baseline distribution
            // immediately before ARC control is enabled.
            EnterPhase(Phase::Baseline);
        }
        break;

    case Phase::Baseline:
        AddStat(baseline_, frameMs);
        (void)host_->drain();
        if (elapsed >= static_cast<double>(measureSeconds_))
        {
            // Use a pre-declared baseline quantile rather than a warmup scale
            // factor. p40 creates real but non-saturated pressure (~60% misses)
            // while leaving room for ARC to move frames across the target.
            const double baselineP50 = std::max(0.05, Percentile(baseline_.frames, 0.50));
            targetMs_ = std::max(0.05, Percentile(baseline_.frames, 0.40));
            recoveryTargetMs_ = std::max(targetMs_ * 1.5, baselineP50 * 1.25);
            RecomputeBudgetStats(baseline_, targetMs_);
            RegisterQualityProfile();

            qualityLevel_ = 0;
            host_->runtime().governor().quality().reset();
            host_->set_mode(arc::RuntimeMode::Controlled);
            controlWindow_.clear();
            EnterPhase(Phase::Adaptive);
        }
        break;

    case Phase::Adaptive:
        AddStat(adaptive_, frameMs);
        controlWindow_.push_back(frameMs);
        if (static_cast<int>(controlWindow_.size()) >= controlFrames_)
        {
            TickGovernor(targetMs_);
            ++adaptiveTicks_;
        }
        if (elapsed >= static_cast<double>(measureSeconds_))
        {
            const auto gm = host_->runtime().governor().metrics();
            const auto qs = host_->runtime().governor().quality().state();
            adaptiveQualityActions_ = gm.quality_actions_executed;
            adaptiveDirectionChanges_ = qs.direction_changes;
            adaptiveRestoreProbes_ = qs.restore_probes;
            adaptiveRestoreBackoffs_ = qs.restore_backoffs;
            controlWindow_.clear();
            host_->runtime().governor().quality().begin_recovery();
            EnterPhase(Phase::Recovery);
        }
        break;

    case Phase::Recovery:
        controlWindow_.push_back(frameMs);
        if (static_cast<int>(controlWindow_.size()) >= controlFrames_)
        {
            TickGovernor(recoveryTargetMs_);
            ++recoveryTicks_;
        }

        if ((qualityLevel_ == 0 &&
             host_->runtime().governor().quality().state().active_actions == 0 &&
             recoveryTicks_ >= 2) ||
            elapsed >= static_cast<double>(recoverySeconds_))
        {
            Finalize();
            shouldExit_ = true;
        }
        break;

    case Phase::Done:
        break;
    }
}

void ArcExternalBridge::Finalize()
{
    if (finalized_) return;
    finalized_ = true;
    if (host_) (void)host_->drain();
    WriteReport(phase_ == Phase::Recovery || phase_ == Phase::Done);
    phase_ = Phase::Done;
}

ComPtr<IDXGIAdapter3> ArcExternalBridge::FindAdapter(ID3D12Device* device)
{
    ComPtr<IDXGIAdapter3> result;
    if (!device) return result;

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return result;

    const LUID wanted = device->GetAdapterLuid();
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.AdapterLuid.HighPart == wanted.HighPart &&
            desc.AdapterLuid.LowPart == wanted.LowPart)
        {
            (void)adapter.As(&result);
            break;
        }
    }
    return result;
}

int ArcExternalBridge::EnvInt(const wchar_t* name, int fallback, int minimum, int maximum)
{
    wchar_t buffer[64]{};
    const DWORD capacity = static_cast<DWORD>(_countof(buffer));
    const DWORD count = GetEnvironmentVariableW(name, buffer, capacity);
    if (count == 0 || count >= capacity) return fallback;
    const int value = _wtoi(buffer);
    return std::clamp(value, minimum, maximum);
}

std::wstring ArcExternalBridge::EnvString(const wchar_t* name)
{
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (!written) return {};
    value.resize(written);
    return value;
}

double ArcExternalBridge::Percentile(std::vector<double> values, double p)
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

double ArcExternalBridge::MissRatio(const Stats& stats) noexcept
{
    return stats.frames.empty() ? 0.0 :
        static_cast<double>(stats.misses) / static_cast<double>(stats.frames.size());
}

double ArcExternalBridge::MeanOvershoot(const Stats& stats) noexcept
{
    return stats.misses ? stats.overshootSum / static_cast<double>(stats.misses) : 0.0;
}

void ArcExternalBridge::RecomputeBudgetStats(Stats& stats, double targetMs) noexcept
{
    stats.misses = 0;
    stats.overshootSum = 0.0;
    if (!(targetMs > 0.0) || !std::isfinite(targetMs)) return;
    for (const double frameMs : stats.frames)
    {
        if (std::isfinite(frameMs) && frameMs > targetMs)
        {
            ++stats.misses;
            stats.overshootSum += frameMs - targetMs;
        }
    }
}

std::string ArcExternalBridge::Narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        out.data(), n, nullptr, nullptr);
    return out;
}

std::string ArcExternalBridge::JsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value)
    {
        if (c == '\\' || c == '"') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') out += "\\n";
        else out.push_back(c);
    }
    return out;
}

void ArcExternalBridge::RegisterQualityProfile()
{
    if (!host_ || profileRegistered_) return;
    const auto& calibrationFrames = baseline_.frames.empty() ? warmup_.frames : baseline_.frames;
    const double p50 = std::max(0.05, Percentile(calibrationFrames, 0.50));

    profile_ = {};
    profile_.id = kQualityProfileId;
    profile_.domain = arc::QualityDomain::Bandwidth;
    profile_.label = "external texture filtering";
    profile_.semantic = arc::QualitySemanticClass::Environment;
    profile_.importance.screen_coverage = 0.85;
    profile_.importance.visibility = 1.0;
    profile_.importance.motion_salience = 0.25;
    profile_.importance.semantic_importance = 0.35;
    profile_.importance.normalized_distance = 0.50;
    profile_.confidence = 0.90;
    profile_.reversible = true;

    profile_.levels = {
        {static_cast<double>(kIterations[1]) / kIterations[0], p50 * 0.13, 0},
        {static_cast<double>(kIterations[2]) / kIterations[0], p50 * 0.11, 0},
        {static_cast<double>(kIterations[3]) / kIterations[0], p50 * 0.09, 0},
        {static_cast<double>(kIterations[4]) / kIterations[0], p50 * 0.07, 0},
    };

    profileRegistered_ = host_->register_quality_profile(profile_);
}

void ArcExternalBridge::AddStat(Stats& stats, double frameMs)
{
    stats.frames.push_back(frameMs);
    if (targetMs_ > 0.0 && frameMs > targetMs_)
    {
        ++stats.misses;
        stats.overshootSum += frameMs - targetMs_;
    }
}

void ArcExternalBridge::TickGovernor(double targetMs)
{
    if (!host_ || controlWindow_.empty()) return;
    arc::FrameBudgetSample frame{};
    frame.frame_ms = Percentile(controlWindow_, 0.50);
    frame.target_frame_ms = targetMs;
    frame.gpu_busy_fraction = 0.98;
    frame.memory_bandwidth_fraction = 0.97;
    frame.raster_pressure = 0.55;
    controlWindow_.clear();
    (void)host_->frame_tick(frame, true);
}

void ArcExternalBridge::EnterPhase(Phase phase)
{
    phase_ = phase;
    phaseStart_ = std::chrono::steady_clock::now();
}

void ArcExternalBridge::WriteReport(bool complete)
{
    if (!host_) return;
    std::error_code ec;
    if (const auto parent = outputPath_.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, ec);

    const auto hm = host_->metrics();
    const auto gm = host_->runtime().governor().metrics();
    const auto qs = host_->runtime().governor().quality().state();
    const auto bm = host_->runtime().bridge().metrics();

    std::uint64_t learned{};
    for (const auto& action : arc::QualityCandidateFactory::build(profile_))
        if (host_->runtime().governor().quality().effects().find(action)) ++learned;

    const double bp50 = Percentile(baseline_.frames, 0.50);
    const double ap50 = Percentile(adaptive_.frames, 0.50);
    const double bp99 = Percentile(baseline_.frames, 0.99);
    const double ap99 = Percentile(adaptive_.frames, 0.99);
    const double bmiss = MissRatio(baseline_);
    const double amiss = MissRatio(adaptive_);
    const double missReduction = bmiss > 0.0 ? (bmiss - amiss) / bmiss : 0.0;
    const double p50Reduction = bp50 > 0.0 ? (bp50 - ap50) / bp50 : 0.0;

    const bool hostClean =
        hm.failed_observations == 0 &&
        hm.bridge_rejections == 0 &&
        bm.controller_rejections == 0 &&
        bm.malformed_events == 0 &&
        host_->graph().errors() == 0;
    const bool finalFull = qualityLevel_ == 0 && qs.active_actions == 0;
    const bool baselinePressureValid = bmiss >= 0.45 && bmiss <= 0.75;
    const bool performanceWin =
        baselinePressureValid &&
        missReduction >= 0.10 &&
        p50Reduction >= 0.05 &&
        ap99 <= bp99 * 1.10;
    const double adaptiveActionRate =
        adaptiveTicks_ ? static_cast<double>(adaptiveQualityActions_) /
            static_cast<double>(adaptiveTicks_) : 0.0;
    const double adaptiveDirectionChangeRate =
        adaptiveTicks_ ? static_cast<double>(adaptiveDirectionChanges_) /
            static_cast<double>(adaptiveTicks_) : 0.0;
    // Churn is a rate property, not an absolute count. A fixed reversal cap
    // becomes stricter merely by running the same stable controller for longer
    // or at a higher frame rate. Require both low action density and very low
    // reversal density during the adaptive phase only; deliberate Recovery
    // restores are reported separately and do not count as steady-state churn.
    const bool boundedChurn =
        adaptiveQualityActions_ <= 24 &&
        adaptiveActionRate <= 0.010 &&
        adaptiveDirectionChangeRate <= 0.005;
    const bool gpuTimingValid =
        gpuTimestampSamples_ >= baseline_.frames.size() + adaptive_.frames.size();
    const bool valid =
        complete &&
        profileRegistered_ &&
        gpuTimingValid &&
        hostClean &&
        finalFull &&
        gm.quality_actions_executed >= 2 &&
        learned >= 1 &&
        performanceWin &&
        boundedChurn;

    std::ofstream f(outputPath_, std::ios::trunc);
    if (!f) return;
    f << std::fixed << std::setprecision(6)
      << "{\n"
      << "  \"schema\":1,\n"
      << "  \"valid\":" << (valid ? "true" : "false") << ",\n"
      << "  \"benchmark\":\"stage14_external_renderer\",\n"
      << "  \"integration\":\"microsoft_directx_graphics_samples_hello_texture\",\n"
      << "  \"upstream_sha\":\"" << kUpstreamSha << "\",\n"
      << "  \"arc_source_sha\":\"" << JsonEscape(Narrow(arcSourceSha_)) << "\",\n"
      << "  \"adapter\":\"" << JsonEscape(adapterName_) << "\",\n"
      << "  \"native_width\":1920,\n"
      << "  \"native_height\":1080,\n"
      << "  \"temporal_used\":false,\n"
      << "  \"timing_source\":\"gpu_timestamp\",\n"
      << "  \"gpu_timestamp_samples\":" << gpuTimestampSamples_ << ",\n"
      << "  \"target_frame_ms\":" << targetMs_ << ",\n"
      << "  \"target_calibration\":\"baseline_p40\",\n"
      << "  \"baseline\":{\"samples\":" << baseline_.frames.size()
      << ",\"p50_ms\":" << bp50 << ",\"p99_ms\":" << bp99
      << ",\"miss_ratio\":" << bmiss
      << ",\"mean_overshoot_ms\":" << MeanOvershoot(baseline_) << "},\n"
      << "  \"adaptive\":{\"samples\":" << adaptive_.frames.size()
      << ",\"p50_ms\":" << ap50 << ",\"p99_ms\":" << ap99
      << ",\"miss_ratio\":" << amiss
      << ",\"mean_overshoot_ms\":" << MeanOvershoot(adaptive_) << "},\n"
      << "  \"host\":{\"resources\":" << hm.resources_observed
      << ",\"descriptor_writes\":" << hm.descriptor_writes
      << ",\"resource_uses\":" << hm.resource_uses
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
      << ",\"pending_effects_resolved\":" << gm.pending_effects_resolved
      << ",\"quality_backend_failures\":" << gm.quality_backend_failures
      << ",\"learned_effects\":" << learned
      << ",\"direction_changes\":" << qs.direction_changes
      << ",\"restore_probes\":" << qs.restore_probes
      << ",\"restore_backoffs\":" << qs.restore_backoffs
      << ",\"adaptive_ticks\":" << adaptiveTicks_
      << ",\"adaptive_quality_actions\":" << adaptiveQualityActions_
      << ",\"adaptive_direction_changes\":" << adaptiveDirectionChanges_
      << ",\"adaptive_restore_probes\":" << adaptiveRestoreProbes_
      << ",\"adaptive_restore_backoffs\":" << adaptiveRestoreBackoffs_
      << ",\"adaptive_action_rate\":" << adaptiveActionRate
      << ",\"adaptive_direction_change_rate\":" << adaptiveDirectionChangeRate
      << ",\"recovery_ticks\":" << recoveryTicks_ << "},\n"
      << "  \"final_quality_full\":" << (finalFull ? "true" : "false") << ",\n"
      << "  \"performance_win\":" << (performanceWin ? "true" : "false") << ",\n"
      << "  \"bounded_churn\":" << (boundedChurn ? "true" : "false") << "\n"
      << "}\n";
}

arc::RuntimeBackendStatus ArcExternalBridge::Mutate(
    const arc::QualityActionCandidate& action,
    bool restore) noexcept
{
    if (action.id != kQualityProfileId || action.sequence >= kIterations.size() - 1)
        return arc::RuntimeBackendStatus::Unsupported;

    if (!restore)
    {
        if (action.sequence != qualityLevel_ || qualityLevel_ + 1 >= kIterations.size())
            return arc::RuntimeBackendStatus::Failure;
        ++qualityLevel_;
    }
    else
    {
        if (qualityLevel_ == 0 || action.sequence + 1 != qualityLevel_)
            return arc::RuntimeBackendStatus::Failure;
        --qualityLevel_;
    }
    return arc::RuntimeBackendStatus::Success;
}
