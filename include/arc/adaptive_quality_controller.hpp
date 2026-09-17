#pragma once

#include "arc/action_effect_tracker.hpp"
#include "arc/adaptive_quality.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arc {

enum class QualityDecisionKind : std::uint8_t {
    None,
    Degrade,
    Restore,
};

struct AdaptiveQualityControllerConfig {
    AdaptiveQualityConfig optimizer{};

    // Hysteresis: require multiple consecutive observations before changing
    // quality.  This keeps one noisy frame from changing the scene.
    std::uint32_t overload_samples_required{3};
    std::uint32_t headroom_samples_required{6};
    std::uint32_t settle_samples_after_change{4};

    // Closed-loop filtering and anti-chatter policy.
    double frame_ewma_alpha{0.35};
    double overload_margin_ms{0.10};
    double extra_restore_headroom_ms{0.25};
    std::uint32_t minimum_hold_samples_after_degrade{8};
    std::uint32_t max_actions_per_decision{1};
    std::uint32_t max_active_actions{16};

    // Stage 9 causal safety.  A quality reduction that does not produce enough
    // measured benefit is rolled back instead of being left active merely
    // because the planner predicted that it would help.
    bool rollback_ineffective_actions{true};
    bool block_failed_action_in_context{true};
    double minimum_observed_gain_fraction{0.30};
    double minimum_observed_gain_ms{0.005};
    double regression_tolerance_ms{0.010};
};

struct QualityDecision {
    QualityDecisionKind kind{QualityDecisionKind::None};
    AdaptiveQualityPlan plan{};
    bool forced_rollback{};
};

struct AdaptiveQualityControllerState {
    bool filter_initialized{};
    double filtered_frame_ms{};
    std::uint32_t overload_samples{};
    std::uint32_t headroom_samples{};
    std::uint32_t settle_remaining{};
    std::uint32_t restore_guard_remaining{};
    std::size_t active_actions{};
    std::size_t blocked_context_actions{};
    std::size_t rollback_queue_depth{};
    std::uint64_t degrade_actions_applied{};
    std::uint64_t restore_actions_applied{};
    std::uint64_t rollback_actions_queued{};
    std::uint64_t rollback_actions_applied{};
    std::uint64_t direction_changes{};
    QualityDecisionKind last_applied_kind{QualityDecisionKind::None};
};

class AdaptiveQualityController final {
public:
    explicit AdaptiveQualityController(AdaptiveQualityControllerConfig config = {});

    // Feed one observed frame-budget sample.  The controller filters frame time,
    // applies hysteresis, enforces quality-ladder order and returns at most
    // max_actions_per_decision actions.  The caller remains responsible for
    // applying actions to the host renderer and reporting the observed effect.
    [[nodiscard]] QualityDecision tick(
        const FrameBudgetSample& sample,
        const std::vector<QualityActionCandidate>& candidates);

    void note_action_applied(
        const QualityActionCandidate& action,
        QualityDecisionKind kind,
        double before_frame_ms,
        double after_frame_ms,
        bool success) noexcept;

    void reset() noexcept;

    [[nodiscard]] std::vector<QualityActionCandidate> active_actions() const;
    [[nodiscard]] AdaptiveQualityControllerState state() const noexcept;
    [[nodiscard]] const ActionEffectTracker& effects() const noexcept { return effects_; }
    [[nodiscard]] const AdaptiveQualityControllerConfig& config() const noexcept { return config_; }

private:
    struct Key {
        std::uint64_t id{};
        std::uint32_t sequence{};
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct Hash {
        std::size_t operator()(const Key& key) const noexcept;
    };
    struct ContextKey {
        Key action{};
        BottleneckClass bottleneck{BottleneckClass::UnknownGpu};
        friend bool operator==(const ContextKey&, const ContextKey&) = default;
    };
    struct ContextHash {
        std::size_t operator()(const ContextKey& key) const noexcept;
    };

    [[nodiscard]] std::vector<QualityActionCandidate> next_degrade_candidates(
        const std::vector<QualityActionCandidate>& candidates,
        BottleneckClass bottleneck) const;
    [[nodiscard]] std::vector<QualityActionCandidate> next_restore_candidates() const;
    [[nodiscard]] BottleneckClass active_context(const QualityActionCandidate& action) const noexcept;
    [[nodiscard]] bool is_blocked(
        const QualityActionCandidate& action,
        BottleneckClass bottleneck) const noexcept;
    void queue_rollback(
        const QualityActionCandidate& action,
        BottleneckClass bottleneck) noexcept;
    void limit_plan(AdaptiveQualityPlan& plan) const noexcept;

    AdaptiveQualityControllerConfig config_{};
    AdaptiveQualityOptimizer optimizer_{};
    ActionEffectTracker effects_{};
    std::unordered_map<Key, QualityActionCandidate, Hash> active_{};
    std::unordered_map<Key, BottleneckClass, Hash> active_contexts_{};
    std::unordered_map<Key, BottleneckClass, Hash> pending_contexts_{};
    std::unordered_set<ContextKey, ContextHash> blocked_contexts_{};
    std::deque<QualityActionCandidate> rollback_queue_{};
    std::unordered_set<Key, Hash> rollback_queued_{};
    std::unordered_set<Key, Hash> rollback_inflight_{};

    bool filter_initialized_{};
    double filtered_frame_ms_{};
    std::uint32_t overload_samples_{};
    std::uint32_t headroom_samples_{};
    std::uint32_t settle_remaining_{};
    std::uint32_t restore_guard_remaining_{};

    std::uint64_t degrade_actions_applied_{};
    std::uint64_t restore_actions_applied_{};
    std::uint64_t rollback_actions_queued_{};
    std::uint64_t rollback_actions_applied_{};
    std::uint64_t direction_changes_{};
    QualityDecisionKind last_applied_kind_{QualityDecisionKind::None};
};

} // namespace arc
