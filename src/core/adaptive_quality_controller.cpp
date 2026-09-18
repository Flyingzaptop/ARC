#include "arc/adaptive_quality_controller.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_map>

namespace arc {
namespace {

std::uint32_t bump_saturating(std::uint32_t value) noexcept {
    return value == std::numeric_limits<std::uint32_t>::max() ? value : value + 1;
}

std::uint32_t multiply_saturating(std::uint32_t value, std::uint32_t factor) noexcept {
    if (value == 0 || factor == 0) return 0;
    const auto max = std::numeric_limits<std::uint32_t>::max();
    return value > max / factor ? max : value * factor;
}

} // namespace

std::size_t AdaptiveQualityController::Hash::operator()(const Key& key) const noexcept {
    const std::size_t h1 = std::hash<std::uint64_t>{}(key.id);
    const std::size_t h2 = std::hash<std::uint32_t>{}(key.sequence);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2));
}

AdaptiveQualityController::AdaptiveQualityController(AdaptiveQualityControllerConfig config)
    : config_(config), optimizer_(config.optimizer) {
    config_.overload_samples_required = std::max<std::uint32_t>(1, config_.overload_samples_required);
    config_.headroom_samples_required = std::max<std::uint32_t>(1, config_.headroom_samples_required);
    config_.frame_ewma_alpha = std::clamp(config_.frame_ewma_alpha, 0.01, 1.0);
    config_.overload_margin_ms = std::max(0.0, config_.overload_margin_ms);
    config_.extra_restore_headroom_ms = std::max(0.0, config_.extra_restore_headroom_ms);
    config_.restore_probe_samples_required = std::max<std::uint32_t>(
        config_.headroom_samples_required, config_.restore_probe_samples_required);
    config_.restore_probe_headroom_fraction = std::clamp(
        config_.restore_probe_headroom_fraction, 0.0, 0.95);
    config_.restore_reversal_window_samples = std::max<std::uint32_t>(
        1, config_.restore_reversal_window_samples);
    config_.restore_backoff_base_samples = std::max<std::uint32_t>(
        1, config_.restore_backoff_base_samples);
    config_.restore_backoff_max_samples = std::max(
        config_.restore_backoff_base_samples, config_.restore_backoff_max_samples);
    config_.max_actions_per_decision = std::max<std::uint32_t>(1, config_.max_actions_per_decision);
    config_.max_active_actions = std::max<std::uint32_t>(1, config_.max_active_actions);
}

std::vector<QualityActionCandidate> AdaptiveQualityController::next_degrade_candidates(
    const std::vector<QualityActionCandidate>& candidates) const {
    std::unordered_map<std::uint64_t, std::uint32_t> max_active_sequence;
    for (const auto& [key, _] : active_) {
        auto [it, inserted] = max_active_sequence.emplace(key.id, key.sequence);
        if (!inserted) it->second = std::max(it->second, key.sequence);
    }

    std::unordered_map<std::uint64_t, std::uint32_t> minimum_available_sequence;
    for (const auto& candidate : candidates) {
        const Key key{candidate.id, candidate.sequence};
        if (active_.contains(key)) continue;
        auto [it, inserted] = minimum_available_sequence.emplace(candidate.id, candidate.sequence);
        if (!inserted) it->second = std::min(it->second, candidate.sequence);
    }

    std::vector<QualityActionCandidate> out;
    out.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        const Key key{candidate.id, candidate.sequence};
        if (active_.contains(key)) continue;

        std::uint32_t expected = candidate.sequence;
        if (const auto active = max_active_sequence.find(candidate.id); active != max_active_sequence.end()) {
            if (active->second == std::numeric_limits<std::uint32_t>::max()) continue;
            expected = active->second + 1;
        } else if (const auto first = minimum_available_sequence.find(candidate.id);
                   first != minimum_available_sequence.end()) {
            expected = first->second;
        }
        if (candidate.sequence == expected) out.push_back(candidate);
    }
    return out;
}

std::vector<QualityActionCandidate> AdaptiveQualityController::next_restore_candidates() const {
    std::unordered_map<std::uint64_t, QualityActionCandidate> highest;
    for (const auto& [key, action] : active_) {
        auto it = highest.find(key.id);
        if (it == highest.end() || action.sequence > it->second.sequence) highest[key.id] = action;
    }

    std::vector<QualityActionCandidate> out;
    out.reserve(highest.size());
    for (const auto& [_, action] : highest) {
        const Key key{action.id, action.sequence};
        const auto penalty = restore_penalties_.find(key);
        if (penalty != restore_penalties_.end() && penalty->second.cooldown > 0) continue;
        out.push_back(action);
    }
    return out;
}

void AdaptiveQualityController::limit_plan(AdaptiveQualityPlan& plan) const noexcept {
    if (plan.actions.size() <= config_.max_actions_per_decision) return;

    const bool prior_shortfall = plan.shortfall;
    const bool intentionally_truncated = true;
    plan.actions.resize(config_.max_actions_per_decision);
    plan.planned_gain_ms = 0.0;
    plan.estimated_visual_cost = 0.0;
    plan.planned_memory_freed_bytes = 0;
    plan.temporal_used = false;
    for (const auto& action : plan.actions) {
        plan.planned_gain_ms += action.expected_ms_gain;
        plan.estimated_visual_cost += action.visual_cost;
        plan.planned_memory_freed_bytes += action.memory_freed_bytes;
        plan.temporal_used = plan.temporal_used || action.temporal_assist || action.domain == QualityDomain::Temporal;
    }
    const bool frame_shortfall = plan.frame_deficit_ms > 0.0 &&
        plan.planned_gain_ms + 1e-9 < plan.frame_deficit_ms;
    // A multi-action plan deliberately throttled to one/few live changes is
    // still incomplete. In particular, memory-emergency callers must not read
    // the truncated iteration as if the complete relief target was satisfied.
    plan.shortfall = prior_shortfall || intentionally_truncated || frame_shortfall;
}

QualityDecision AdaptiveQualityController::tick(
    const FrameBudgetSample& sample,
    const std::vector<QualityActionCandidate>& candidates) {
    QualityDecision out{};

    if (!std::isfinite(sample.frame_ms) || !std::isfinite(sample.target_frame_ms) ||
        sample.frame_ms <= 0.0 || sample.target_frame_ms <= 0.0) {
        return out;
    }

    if (!filter_initialized_) {
        filtered_frame_ms_ = sample.frame_ms;
        filter_initialized_ = true;
    } else {
        const double a = config_.frame_ewma_alpha;
        filtered_frame_ms_ = a * sample.frame_ms + (1.0 - a) * filtered_frame_ms_;
    }

    if (restore_guard_remaining_ > 0) --restore_guard_remaining_;

    for (auto& [_, penalty] : restore_penalties_) {
        if (penalty.cooldown > 0) --penalty.cooldown;
    }
    for (auto it = recent_restores_.begin(); it != recent_restores_.end();) {
        it->second = bump_saturating(it->second);
        if (it->second > config_.restore_reversal_window_samples) {
            // A restore that survives the reversal window is considered stable;
            // forgive any older backoff history for that action.
            restore_penalties_.erase(it->first);
            it = recent_restores_.erase(it);
        } else {
            ++it;
        }
    }

    if (settle_remaining_ > 0) {
        --settle_remaining_;
        overload_samples_ = 0;
        headroom_samples_ = 0;
        return out;
    }

    FrameBudgetSample filtered = sample;
    filtered.frame_ms = filtered_frame_ms_;

    const bool overloaded =
        filtered_frame_ms_ > sample.target_frame_ms + config_.overload_margin_ms;
    const double restore_reserve = config_.optimizer.restoration_headroom_ms +
        config_.extra_restore_headroom_ms;
    const bool has_headroom =
        filtered_frame_ms_ + restore_reserve < sample.target_frame_ms;

    overload_samples_ = overloaded ? bump_saturating(overload_samples_) : 0;
    headroom_samples_ = has_headroom ? bump_saturating(headroom_samples_) : 0;

    if (overload_samples_ >= config_.overload_samples_required &&
        active_.size() < config_.max_active_actions) {
        auto available = next_degrade_candidates(candidates);
        std::vector<QualityActionCandidate> calibrated;
        calibrated.reserve(available.size());
        for (const auto& candidate : available) calibrated.push_back(effects_.calibrate(candidate));

        out.plan = optimizer_.plan_degrade(filtered, calibrated);
        limit_plan(out.plan);
        if (!out.plan.actions.empty()) {
            out.kind = QualityDecisionKind::Degrade;
            overload_samples_ = 0;
            headroom_samples_ = 0;
        }
        return out;
    }

    if (headroom_samples_ >= config_.headroom_samples_required &&
        restore_guard_remaining_ == 0 && !active_.empty()) {
        auto restorable = next_restore_candidates();
        std::vector<QualityActionCandidate> calibrated;
        calibrated.reserve(restorable.size());
        for (const auto& action : restorable) {
            auto estimate = effects_.calibrate(action);
            if (const auto restore = restore_effects_.find(action)) {
                const double sigma = std::sqrt(std::max(0.0, restore->variance_ms2));
                const double conservative_restore_cost =
                    std::max(0.0, restore->mean_gain_ms + sigma);
                estimate.expected_ms_gain =
                    std::max(estimate.expected_ms_gain, conservative_restore_cost);
            }
            calibrated.push_back(estimate);
        }

        out.plan = optimizer_.plan_restore(filtered, calibrated);
        limit_plan(out.plan);

        if (out.plan.actions.empty() &&
            headroom_samples_ >= config_.restore_probe_samples_required &&
            filtered_frame_ms_ <= sample.target_frame_ms *
                (1.0 - config_.restore_probe_headroom_fraction) &&
            !calibrated.empty()) {
            // A stale scene-dependent estimate can otherwise make the final
            // reversible step impossible to restore. Probe exactly one cheap
            // step, but only after sustained deep headroom. Failed probes are
            // quarantined by the per-action reversal backoff below.
            const auto probe = std::min_element(
                calibrated.begin(), calibrated.end(), [](const auto& a, const auto& b) {
                    if (a.expected_ms_gain != b.expected_ms_gain) return a.expected_ms_gain < b.expected_ms_gain;
                    if (a.visual_cost != b.visual_cost) return a.visual_cost > b.visual_cost;
                    if (a.id != b.id) return a.id < b.id;
                    return a.sequence > b.sequence;
                });
            if (probe != calibrated.end()) {
                out.plan.actions.push_back(*probe);
                out.plan.planned_gain_ms = probe->expected_ms_gain;
                out.plan.estimated_visual_cost = probe->visual_cost;
                out.plan.planned_memory_freed_bytes = probe->memory_freed_bytes;
                ++restore_probes_;
            }
        }

        if (!out.plan.actions.empty()) {
            out.kind = QualityDecisionKind::Restore;
            headroom_samples_ = 0;
            overload_samples_ = 0;
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
    if (!success || kind == QualityDecisionKind::None) return;

    const auto previous_kind = last_applied_kind_;
    const Key key{action.id, action.sequence};
    if (kind == QualityDecisionKind::Degrade) {
        active_[key] = action;

        if (const auto recent = recent_restores_.find(key);
            recent != recent_restores_.end() &&
            recent->second <= config_.restore_reversal_window_samples) {
            auto& penalty = restore_penalties_[key];
            penalty.failures = bump_saturating(penalty.failures);
            std::uint32_t cooldown = config_.restore_backoff_base_samples;
            for (std::uint32_t i = 1;
                 i < penalty.failures && cooldown < config_.restore_backoff_max_samples;
                 ++i) {
                cooldown = std::min(
                    config_.restore_backoff_max_samples,
                    multiply_saturating(cooldown, 2));
            }
            penalty.cooldown = std::max(penalty.cooldown, cooldown);
            recent_restores_.erase(recent);
            ++restore_backoffs_;
        }

        const double observed_gain = std::isfinite(before_frame_ms) && std::isfinite(after_frame_ms)
            ? std::max(0.0, before_frame_ms - after_frame_ms)
            : 0.0;
        effects_.record(action, observed_gain);
        ++degrade_actions_applied_;

        // A degrade immediately following a restore is evidence that the
        // restore estimate was optimistic or that we are hovering near the
        // frame target. Extend the normal hold window to prevent ping-pong.
        const auto hold = previous_kind == QualityDecisionKind::Restore
            ? multiply_saturating(config_.minimum_hold_samples_after_degrade, 4)
            : config_.minimum_hold_samples_after_degrade;
        restore_guard_remaining_ = std::max(restore_guard_remaining_, hold);
    } else if (kind == QualityDecisionKind::Restore) {
        active_.erase(key);
        const double observed_cost = std::isfinite(before_frame_ms) && std::isfinite(after_frame_ms)
            ? std::max(0.0, after_frame_ms - before_frame_ms)
            : 0.0;
        // Keep restore cost independent from degradation gain. They are
        // directionally related, but scene/frequency/scheduling noise can make
        // their measured distributions asymmetric. Restore planning uses the
        // conservative upper estimate of this cost instead of contaminating
        // the degradation-benefit model.
        restore_effects_.record(action, observed_cost);
        recent_restores_[key] = 0;
        ++restore_actions_applied_;
    }

    if (previous_kind != QualityDecisionKind::None && previous_kind != kind) {
        ++direction_changes_;
    }
    last_applied_kind_ = kind;
    settle_remaining_ = config_.settle_samples_after_change;
    overload_samples_ = 0;
    headroom_samples_ = 0;
}

void AdaptiveQualityController::reset() noexcept {
    active_.clear();
    restore_penalties_.clear();
    recent_restores_.clear();
    effects_.clear();
    restore_effects_.clear();
    filter_initialized_ = false;
    filtered_frame_ms_ = 0.0;
    overload_samples_ = 0;
    headroom_samples_ = 0;
    settle_remaining_ = 0;
    restore_guard_remaining_ = 0;
    degrade_actions_applied_ = 0;
    restore_actions_applied_ = 0;
    direction_changes_ = 0;
    restore_probes_ = 0;
    restore_backoffs_ = 0;
    last_applied_kind_ = QualityDecisionKind::None;
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

AdaptiveQualityControllerState AdaptiveQualityController::state() const noexcept {
    AdaptiveQualityControllerState out{};
    out.filter_initialized = filter_initialized_;
    out.filtered_frame_ms = filtered_frame_ms_;
    out.overload_samples = overload_samples_;
    out.headroom_samples = headroom_samples_;
    out.settle_remaining = settle_remaining_;
    out.restore_guard_remaining = restore_guard_remaining_;
    out.active_actions = active_.size();
    out.degrade_actions_applied = degrade_actions_applied_;
    out.restore_actions_applied = restore_actions_applied_;
    out.direction_changes = direction_changes_;
    out.restore_probes = restore_probes_;
    out.restore_backoffs = restore_backoffs_;
    out.last_applied_kind = last_applied_kind_;
    return out;
}

} // namespace arc
