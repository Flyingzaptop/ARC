#include "arc/adaptive_quality_controller.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace arc {

std::size_t AdaptiveQualityController::Hash::operator()(const Key& key) const noexcept {
    const std::size_t h1 = std::hash<std::uint64_t>{}(key.id);
    const std::size_t h2 = std::hash<std::uint32_t>{}(key.sequence);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2));
}

AdaptiveQualityController::AdaptiveQualityController(AdaptiveQualityControllerConfig config)
    : config_(config), optimizer_(config.optimizer) {
    config_.overload_samples_required = std::max<std::uint32_t>(1, config_.overload_samples_required);
    config_.headroom_samples_required = std::max<std::uint32_t>(1, config_.headroom_samples_required);
}

QualityDecision AdaptiveQualityController::tick(
    const FrameBudgetSample& sample,
    const std::vector<QualityActionCandidate>& candidates) {
    QualityDecision out{};

    if (settle_remaining_ > 0) {
        --settle_remaining_;
        return out;
    }

    const bool overloaded = sample.frame_ms > sample.target_frame_ms;
    const bool has_headroom = sample.frame_ms + config_.optimizer.restoration_headroom_ms < sample.target_frame_ms;

    overload_samples_ = overloaded ? overload_samples_ + 1 : 0;
    headroom_samples_ = has_headroom ? headroom_samples_ + 1 : 0;

    if (overload_samples_ >= config_.overload_samples_required) {
        std::vector<QualityActionCandidate> calibrated;
        calibrated.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            const Key key{candidate.id, candidate.sequence};
            if (active_.contains(key)) continue;
            calibrated.push_back(effects_.calibrate(candidate));
        }
        out.plan = optimizer_.plan_degrade(sample, calibrated);
        if (!out.plan.actions.empty()) {
            out.kind = QualityDecisionKind::Degrade;
            overload_samples_ = 0;
        }
        return out;
    }

    if (headroom_samples_ >= config_.headroom_samples_required && !active_.empty()) {
        std::vector<QualityActionCandidate> active;
        active.reserve(active_.size());
        for (const auto& [_, action] : active_) active.push_back(effects_.calibrate(action));
        out.plan = optimizer_.plan_restore(sample, active);
        if (!out.plan.actions.empty()) {
            out.kind = QualityDecisionKind::Restore;
            headroom_samples_ = 0;
        }
    }
    return out;
}

void AdaptiveQualityController::note_action_applied(
    const QualityActionCandidate& action,
    QualityDecisionKind kind,
    double before_frame_ms,
    double after_frame_ms,
    bool success) noexcept {
    if (!success) return;
    const Key key{action.id, action.sequence};
    if (kind == QualityDecisionKind::Degrade) {
        active_[key] = action;
        const double observed_gain = std::isfinite(before_frame_ms) && std::isfinite(after_frame_ms)
            ? std::max(0.0, before_frame_ms - after_frame_ms)
            : 0.0;
        effects_.record(action, observed_gain);
        settle_remaining_ = config_.settle_samples_after_change;
    } else if (kind == QualityDecisionKind::Restore) {
        active_.erase(key);
        settle_remaining_ = config_.settle_samples_after_change;
    }
}

void AdaptiveQualityController::reset() noexcept {
    active_.clear();
    effects_.clear();
    overload_samples_ = 0;
    headroom_samples_ = 0;
    settle_remaining_ = 0;
}

std::vector<QualityActionCandidate> AdaptiveQualityController::active_actions() const {
    std::vector<QualityActionCandidate> out;
    out.reserve(active_.size());
    for (const auto& [_, action] : active_) out.push_back(action);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.id != b.id) return a.id < b.id;
        return a.sequence < b.sequence;
    });
    return out;
}

} // namespace arc
