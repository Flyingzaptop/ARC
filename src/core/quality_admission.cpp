#include "arc/quality_admission.hpp"

#include <algorithm>
#include <cmath>

namespace arc {
namespace {

double clamp01(double value) noexcept {
    if (!std::isfinite(value)) return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

double memory_pressure(const FrameBudgetSample& frame) noexcept {
    if (frame.local_budget_bytes == 0) return 0.0;
    return std::clamp(
        static_cast<double>(frame.local_usage_bytes) /
            static_cast<double>(frame.local_budget_bytes),
        0.0,
        4.0);
}

} // namespace

QualityAdmissionController::QualityAdmissionController(QualityAdmissionConfig config)
    : config_(config) {
    config_.memory_pressure_enter = std::clamp(config_.memory_pressure_enter, 0.0, 1.0);
    config_.memory_pressure_emergency = std::clamp(
        std::max(config_.memory_pressure_enter, config_.memory_pressure_emergency), 0.0, 1.0);
    config_.frame_pressure_enter = std::max(1.0, config_.frame_pressure_enter);
    config_.step1_threshold = clamp01(config_.step1_threshold);
    config_.step2_threshold = std::clamp(
        std::max(config_.step1_threshold, config_.step2_threshold), 0.0, 1.0);
    config_.step3_threshold = std::clamp(
        std::max(config_.step2_threshold, config_.step3_threshold), 0.0, 1.0);
}

bool QualityAdmissionController::semantic_protected(QualitySemanticClass semantic) const noexcept {
    switch (semantic) {
    case QualitySemanticClass::Ui: return config_.protect_ui;
    case QualitySemanticClass::Face: return config_.protect_faces;
    case QualitySemanticClass::PlayerWeapon: return config_.protect_player_weapon;
    default: return false;
    }
}

QualityAdmissionDecision QualityAdmissionController::decide(
    const QualityAdmissionResource& resource,
    const FrameBudgetSample& frame) const {
    QualityAdmissionDecision out{};
    if (resource.profile.id == 0 || resource.profile.levels.empty() || !resource.controllable) return out;
    if (!resource.profile.reversible || resource.profile.temporal_assist ||
        resource.profile.domain == QualityDomain::Temporal) return out;

    out.valid = true;
    out.importance_score = ResourceImportanceEstimator::score(resource.profile.importance);
    out.protected_semantic = semantic_protected(resource.profile.semantic);

    const double mem = memory_pressure(frame);
    const double mem_pressure = mem <= config_.memory_pressure_enter ? 0.0 :
        clamp01((mem - config_.memory_pressure_enter) /
                std::max(1e-6, config_.memory_pressure_emergency - config_.memory_pressure_enter));

    double frame_pressure = 0.0;
    if (frame.target_frame_ms > 0.0 && std::isfinite(frame.frame_ms) && std::isfinite(frame.target_frame_ms)) {
        const double ratio = frame.frame_ms / frame.target_frame_ms;
        if (ratio > config_.frame_pressure_enter) {
            frame_pressure = clamp01((ratio - config_.frame_pressure_enter) / 0.45);
        }
    }

    const double raw_pressure = std::max(mem_pressure, frame_pressure);
    const double importance_guard = 1.0 - 0.85 * clamp01(out.importance_score);
    out.pressure_score = clamp01(raw_pressure * importance_guard);

    // UI/face/weapon remain full quality even during ordinary pressure.  The
    // memory governor can still fail closed / shed other resources first.
    if (out.protected_semantic) return out;

    std::uint32_t desired = 0;
    if (out.pressure_score >= config_.step3_threshold) desired = 3;
    else if (out.pressure_score >= config_.step2_threshold) desired = 2;
    else if (out.pressure_score >= config_.step1_threshold) desired = 1;

    const auto candidates = QualityCandidateFactory::build(resource.profile);
    if (candidates.empty() || desired == 0) return out;

    desired = std::min<std::uint32_t>(desired, static_cast<std::uint32_t>(candidates.size()));
    out.initial_actions.reserve(desired);
    for (std::uint32_t i = 0; i < desired; ++i) {
        const auto& action = candidates[i];
        // Strict ladder prefix only.  The factory can omit malformed levels;
        // admission never skips a sequence in order to reach a deeper step.
        if (action.sequence != i) break;
        out.initial_actions.push_back(action);
        out.estimated_bytes_saved += action.memory_freed_bytes;
    }

    out.admitted_level = static_cast<std::uint32_t>(out.initial_actions.size());
    out.reduced = out.admitted_level != 0;
    if (out.reduced) {
        const auto index = std::min<std::size_t>(out.admitted_level - 1, resource.profile.levels.size() - 1);
        out.retained_quality = std::clamp(resource.profile.levels[index].retained_quality, 0.0, 1.0);
    }
    return out;
}

} // namespace arc
