#pragma once

#include "arc/action_effect_tracker.hpp"
#include "arc/adaptive_quality.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
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

    // Restore probes are only a deadlock escape hatch. They require sustained
    // deep headroom; a probe that is reversed shortly afterwards receives an
    // exponential per-action cooldown so scene-dependent effect estimates
    // cannot ping-pong quality around the frame target.
    std::uint32_t restore_probe_samples_required{12};
    double restore_probe_headroom_fraction{0.08};
    std::uint32_t restore_reversal_window_samples{24};
    std::uint32_t restore_backoff_base_samples{48};
    std::uint32_t restore_backoff_max_samples{768};

    std::uint32_t max_actions_per_decision{1};
    std::uint32_t max_active_actions{16};
};

struct QualityDecision {
    QualityDecisionKind kind{QualityDecisionKind::None};
    AdaptiveQualityPlan plan{};
};

struct AdaptiveQualityControllerState {
    bool filter_initialized{};
    double filtered_frame_ms{};
    std::uint32_t overload_samples{};
    std::uint32_t headroom_samples{};
    std::uint32_t settle_remaining{};
    std::uint32_t restore_guard_remaining{};
    std::size_t active_actions{};
    std::uint64_t degrade_actions_applied{};
    std::uint64_t restore_actions_applied{};
    std::uint64_t direction_changes{};
    std::uint64_t restore_probes{};
    std::uint64_t restore_backoffs{};
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

    // Enter an explicit recovery/rollback phase. Preserve active actions and
    // learned effects, but release temporary anti-chatter state that is only
    // meaningful while holding a steady adaptive operating point.
    void begin_recovery() noexcept;
    void reset() noexcept;

    [[nodiscard]] std::vector<QualityActionCandidate> active_actions() const;
    [[nodiscard]] AdaptiveQualityControllerState state() const noexcept;
    [[nodiscard]] const ActionEffectTracker& effects() const noexcept { return effects_; }
    [[nodiscard]] const ActionEffectTracker& restore_effects() const noexcept { return restore_effects_; }
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

    [[nodiscard]] std::vector<QualityActionCandidate> next_degrade_candidates(
        const std::vector<QualityActionCandidate>& candidates) const;
    [[nodiscard]] std::vector<QualityActionCandidate> next_restore_candidates() const;
    void limit_plan(AdaptiveQualityPlan& plan) const noexcept;

    AdaptiveQualityControllerConfig config_{};
    AdaptiveQualityOptimizer optimizer_{};
    ActionEffectTracker effects_{};
    ActionEffectTracker restore_effects_{};
    struct RestorePenalty {
        std::uint32_t failures{};
        std::uint32_t cooldown{};
    };

    std::unordered_map<Key, QualityActionCandidate, Hash> active_{};
    std::unordered_map<Key, RestorePenalty, Hash> restore_penalties_{};
    std::unordered_map<Key, std::uint32_t, Hash> recent_restores_{};

    bool recovery_mode_{};
    bool filter_initialized_{};
    double filtered_frame_ms_{};
    std::uint32_t overload_samples_{};
    std::uint32_t headroom_samples_{};
    std::uint32_t settle_remaining_{};
    std::uint32_t restore_guard_remaining_{};

    std::uint64_t degrade_actions_applied_{};
    std::uint64_t restore_actions_applied_{};
    std::uint64_t direction_changes_{};
    std::uint64_t restore_probes_{};
    std::uint64_t restore_backoffs_{};
    QualityDecisionKind last_applied_kind_{QualityDecisionKind::None};
};

} // namespace arc
