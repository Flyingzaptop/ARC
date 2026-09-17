#pragma once

#include "arc/adaptive_quality.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace arc {

struct ActionEffectKey {
    std::uint64_t id{};
    std::uint32_t sequence{};
    friend bool operator==(const ActionEffectKey&, const ActionEffectKey&) = default;
};

struct ContextualActionEffectKey {
    ActionEffectKey action{};
    BottleneckClass bottleneck{BottleneckClass::UnknownGpu};
    friend bool operator==(const ContextualActionEffectKey&, const ContextualActionEffectKey&) = default;
};

struct ActionEffectStats {
    std::uint64_t samples{};
    std::uint64_t negative_samples{};
    double mean_gain_ms{};
    double variance_ms2{};
    double confidence{};
    double negative_fraction{};
};

class ActionEffectTracker final {
public:
    explicit ActionEffectTracker(double prior_weight = 3.0) noexcept;

    // Global, context-agnostic observation.  Kept for callers that do not have
    // bottleneck attribution and for a weak transfer prior across contexts.
    void record(const QualityActionCandidate& action, double observed_gain_ms) noexcept;

    // Contextual observation.  The sample is learned both globally and for the
    // exact bottleneck, preventing an action that is great in one scene from
    // being blindly trusted in another.
    void record(
        const QualityActionCandidate& action,
        BottleneckClass bottleneck,
        double observed_gain_ms) noexcept;

    [[nodiscard]] std::optional<ActionEffectStats> find(
        const QualityActionCandidate& action) const noexcept;
    [[nodiscard]] std::optional<ActionEffectStats> find(
        const QualityActionCandidate& action,
        BottleneckClass bottleneck) const noexcept;

    [[nodiscard]] QualityActionCandidate calibrate(
        const QualityActionCandidate& action) const noexcept;
    [[nodiscard]] QualityActionCandidate calibrate(
        const QualityActionCandidate& action,
        BottleneckClass bottleneck) const noexcept;

    void clear() noexcept {
        global_states_.clear();
        contextual_states_.clear();
    }

private:
    struct RunningState {
        std::uint64_t samples{};
        std::uint64_t negative_samples{};
        double mean{};
        double m2{};
    };
    struct ActionHash {
        std::size_t operator()(const ActionEffectKey& key) const noexcept;
    };
    struct ContextHash {
        std::size_t operator()(const ContextualActionEffectKey& key) const noexcept;
    };

    static void update(RunningState& state, double observed_gain_ms) noexcept;
    [[nodiscard]] ActionEffectStats stats(const RunningState& state) const noexcept;
    [[nodiscard]] QualityActionCandidate blend(
        const QualityActionCandidate& action,
        const ActionEffectStats& stats,
        double transfer_scale) const noexcept;

    std::unordered_map<ActionEffectKey, RunningState, ActionHash> global_states_{};
    std::unordered_map<ContextualActionEffectKey, RunningState, ContextHash> contextual_states_{};
    double prior_weight_{3.0};
};

} // namespace arc
