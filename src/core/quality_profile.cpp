#include "arc/quality_profile.hpp"

#include <algorithm>
#include <cmath>

namespace arc {
namespace {
double clamp01(double value) noexcept {
    if (!std::isfinite(value)) return 0.0;
    return std::clamp(value, 0.0, 1.0);
}
}

double QualityCandidateFactory::semantic_protection(QualitySemanticClass semantic) noexcept {
    switch (semantic) {
    case QualitySemanticClass::Ui: return 2.20;
    case QualitySemanticClass::Face: return 2.00;
    case QualitySemanticClass::PlayerWeapon: return 1.90;
    case QualitySemanticClass::Character: return 1.45;
    case QualitySemanticClass::Effect: return 1.15;
    case QualitySemanticClass::Environment: return 0.90;
    case QualitySemanticClass::Foliage: return 0.75;
    case QualitySemanticClass::Generic: return 1.00;
    }
    return 1.0;
}

std::vector<QualityActionCandidate> QualityCandidateFactory::build(
    const QualityResourceProfile& profile) {
    std::vector<QualityActionCandidate> out;
    if (profile.id == 0 || profile.levels.empty()) return out;

    const double importance = ResourceImportanceEstimator::score(profile.importance);
    const double semantic = semantic_protection(profile.semantic);
    const double confidence = clamp01(profile.confidence);

    double previous_quality = 1.0;
    out.reserve(profile.levels.size());
    for (std::size_t i = 0; i < profile.levels.size(); ++i) {
        const auto& level = profile.levels[i];
        const double retained = clamp01(level.retained_quality);
        if (retained >= previous_quality - 1e-9) continue;
        const double quality_drop = previous_quality - retained;

        QualityActionCandidate candidate{};
        candidate.id = profile.id;
        candidate.domain = profile.domain;
        candidate.label = profile.label + " quality step " + std::to_string(i);
        candidate.expected_ms_gain = std::max(0.0, level.expected_ms_gain);
        // Visible/semantic resources are deliberately expensive to degrade.
        candidate.visual_cost = std::max(0.001, quality_drop * (0.15 + importance) * semantic);
        candidate.confidence = confidence;
        candidate.memory_freed_bytes = level.memory_freed_bytes;
        candidate.reversible = profile.reversible;
        candidate.temporal_assist = profile.temporal_assist || profile.domain == QualityDomain::Temporal;
        candidate.sequence = static_cast<std::uint32_t>(i);
        out.push_back(std::move(candidate));
        previous_quality = retained;
    }
    return out;
}

} // namespace arc
