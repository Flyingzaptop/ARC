#pragma once

#include "arc/action_effect_tracker.hpp"
#include "arc/adaptive_quality.hpp"

#include <cstdint>
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
    std::uint32_t overload_samples_required{3};
    std::uint32_t headroom_samples_required{6};
    std::uint32_t settle_samples_after_change{4};
};

struct QualityDecision {
    QualityDecisionKind kind{QualityDecisionKind::None};
    AdaptiveQualityPlan plan{};
};

class AdaptiveQualityController final {
public:
    explicit AdaptiveQualityController(AdaptiveQualityControllerConfig config = {});

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

    AdaptiveQualityControllerConfig config_{};
    AdaptiveQualityOptimizer optimizer_{};
    ActionEffectTracker effects_{};
    std::unordered_map<Key, QualityActionCandidate, Hash> active_{};
    std::uint32_t overload_samples_{};
    std::uint32_t headroom_samples_{};
    std::uint32_t settle_remaining_{};
};

} // namespace arc
