#include "arc/visual_importance.hpp"

#include <algorithm>
#include <cmath>

namespace arc {
namespace {

double clamp01(double value) noexcept {
    if (!std::isfinite(value)) return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

bool valid_probability(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

double phase_weight(VisibilityPhase phase) noexcept {
    switch (phase) {
    case VisibilityPhase::Visible: return 1.00;
    case VisibilityPhase::Entering: return 1.00;
    case VisibilityPhase::Leaving: return 0.82;
    case VisibilityPhase::Hidden: return 0.08;
    case VisibilityPhase::Unknown: return 0.55;
    }
    return 0.55;
}

}  // namespace

VisualImportanceModel::VisualImportanceModel(VisualImportanceConfig config)
    : config_(config) {
    const bool valid =
        valid_probability(config_.uncertainty_reserve) &&
        valid_probability(config_.peripheral_floor) &&
        valid_probability(config_.future_2f_weight) &&
        valid_probability(config_.future_8f_weight) &&
        valid_probability(config_.future_30f_weight);
    if (!valid) config_ = VisualImportanceConfig{};
}

VisualImportance VisualImportanceModel::evaluate(
    const TemporalVisibilityState& visibility,
    const VisualImportanceHint& hint) const noexcept {
    VisualImportance out{};
    out.id = visibility.id;

    const double current = clamp01(visibility.estimated_visible_coverage);
    const double p2 = clamp01(visibility.predicted_2f);
    const double p8 = clamp01(visibility.predicted_8f);
    const double p30 = clamp01(visibility.predicted_30f);

    const double future_coverage = std::max({
        current,
        config_.future_2f_weight * p2,
        config_.future_8f_weight * p8,
        config_.future_30f_weight * p30
    });

    // Perception is not linear in pixel area: sqrt prevents small but real
    // contributions from becoming numerically negligible too early.
    out.coverage_component = clamp01(std::sqrt(future_coverage));
    out.visibility_component = phase_weight(visibility.phase);
    out.temporal_component = clamp01(std::max(visibility.temporal_relevance, future_coverage));

    bool unknown = false;
    if (hint.screen_x && hint.screen_y &&
        std::isfinite(*hint.screen_x) && std::isfinite(*hint.screen_y)) {
        const double radial = std::min(
            1.0,
            std::sqrt((*hint.screen_x) * (*hint.screen_x) + (*hint.screen_y) * (*hint.screen_y)) /
                std::sqrt(2.0));
        out.centrality_component =
            clamp01(config_.peripheral_floor + (1.0 - config_.peripheral_floor) * (1.0 - radial));
    } else {
        out.centrality_component = 1.0;
        unknown = true;
    }

    if (hint.perceptual_sensitivity && valid_probability(*hint.perceptual_sensitivity)) {
        out.sensitivity_component = *hint.perceptual_sensitivity;
    } else {
        out.sensitivity_component = 1.0;
        unknown = true;
    }

    if (hint.composition_relevance && valid_probability(*hint.composition_relevance)) {
        out.composition_component = *hint.composition_relevance;
    } else {
        out.composition_component = 1.0;
        unknown = true;
    }

    out.confidence = clamp01(visibility.confidence);
    const double base =
        out.coverage_component *
        out.visibility_component *
        (0.55 + 0.45 * out.temporal_component) *
        out.centrality_component *
        out.sensitivity_component *
        out.composition_component;

    // Uncertainty increases caution, not optimism. It cannot make unknown work
    // look cheaper merely because ARC lacks evidence.
    const double uncertain_mass = std::max(out.coverage_component, 0.05);
    out.uncertainty_component =
        clamp01((1.0 - out.confidence) * config_.uncertainty_reserve * uncertain_mass);

    out.score = clamp01(base + out.uncertainty_component);
    out.conservative_unknowns = unknown || out.confidence < 0.5;
    return out;
}

}  // namespace arc
