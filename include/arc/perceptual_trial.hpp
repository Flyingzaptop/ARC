#pragma once
#include "arc/visual_importance.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace arc {
enum class PerceptualMechanism { SamplerMipBias, SrvMipRange, HostDefined };
struct PerceptualCapability {
    std::uint64_t action{}, target{}, generation{};
    PerceptualMechanism mechanism{PerceptualMechanism::HostDefined};
    bool supported{}, reversible{}, synchronized{}, reference_capture{};
    // Stronger host contract: exploratory changes cannot affect a live displayed
    // frame or shared game state. Defaults false for all existing adapters.
    bool isolated_probe{};
};
struct PerceptualCandidate {
    PerceptualCapability capability;
    VisualImportance importance;
    double measured_cost_ms{}, expected_gain_ms{};
};
enum class ProbePhase { ReferenceBefore, Modified, ReferenceAfter };
struct ProbeCapture {
    // All three captures must represent the same frozen/reproducible input state.
    // This is a host evidence contract, not an engine name or a frame counter.
    std::uint64_t state_key{}, generation{};
    std::uint32_t width{}, height{};
    bool readback_complete{}, linear_rgb{};
    std::vector<float> rgb;
    std::vector<double> gpu_ms;
};
struct PerceptualGuardConfig {
    std::size_t max_pixels{3840u*2160u}, max_timing_samples{128};
    std::size_t minimum_timing_samples{7};
    double reference_mean_error{0.0005}, reference_peak_error{0.004};
    double modified_mean_error{0.002}, modified_peak_error{0.04};
    double modified_tile_error{0.008};
    double maximum_baseline_drift{0.10}, minimum_gain_ms{0.02}, minimum_gain_fraction{0.02};
    double maximum_importance{0.35}, minimum_confidence{0.75};
};
enum class CriticReason { Accepted, InvalidCapture, ReferenceDrift, ImageDamage, InvalidTiming, TimingDrift, NoBenefit };
struct PerceptualVerdict {
    CriticReason reason{CriticReason::InvalidCapture};
    double reference_mean{}, reference_peak{}, modified_mean{}, modified_peak{}, modified_tile{};
    double before_ms{}, modified_ms{}, after_ms{}, gain_ms{};
    [[nodiscard]] bool accepted() const noexcept { return reason==CriticReason::Accepted; }
};
// Independent of candidate importance, expected gain, scene names and controller state.
class PerceptualCritic {
public:
    explicit PerceptualCritic(PerceptualGuardConfig config = {});
    [[nodiscard]] PerceptualVerdict evaluate(const ProbeCapture&,const ProbeCapture&,const ProbeCapture&) const;
    [[nodiscard]] const PerceptualGuardConfig& config() const noexcept {return config_;}
private:
    PerceptualGuardConfig config_;
};
class PerceptualProbeHost {
public:
    virtual ~PerceptualProbeHost()=default;
    // prepare pins target identity and saves the ORIGINAL state for idempotent restore.
    virtual bool prepare(const PerceptualCapability&)=0;
    virtual bool apply(const PerceptualCapability&)=0;
    virtual bool restore(const PerceptualCapability&) noexcept=0;
    virtual std::optional<ProbeCapture> capture(ProbePhase)=0;
    virtual void finish() noexcept=0;
};
enum class TrialStatus { NotAdmitted, Busy, Faulted, ProbeUnavailable, Rejected, Retained, RolledBack, RollbackFailed };
struct PerceptualTrialResult {
    TrialStatus status{TrialStatus::NotAdmitted};
    PerceptualVerdict verdict;
    bool reference_restored{};
};
// Single-action serialized transaction. Host callbacks may fail/throw; rollback
// failure latches a fault. This never reduces critic thresholds to get a gain.
class PerceptualTrialController {
public:
    explicit PerceptualTrialController(PerceptualGuardConfig config = {});
    [[nodiscard]] std::optional<std::size_t> choose(std::span<const PerceptualCandidate>) const;
    PerceptualTrialResult trial(PerceptualProbeHost&,const PerceptualCandidate&);
    bool restore(PerceptualProbeHost&) noexcept;
    [[nodiscard]] bool faulted() const noexcept {return faulted_;}
    [[nodiscard]] bool active() const noexcept {return active_.has_value();}
    // No blind reset: recovery must actually restore the pinned original state.
    bool recover(PerceptualProbeHost&) noexcept;
private:
    PerceptualCritic critic_;
    std::optional<PerceptualCapability> active_;
    PerceptualProbeHost* owner_{};
    bool faulted_{}, busy_{};
    bool admitted(const PerceptualCandidate&) const noexcept;
};
} // namespace arc
