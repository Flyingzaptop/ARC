#include "arc/render_candidate_catalog.hpp"

#include <algorithm>

namespace arc {

std::vector<QualityActionCandidate> RenderCandidateCatalog::build(
    const RenderPassTimings& timings,
    const std::vector<RenderResourceProfile>& resources) {
    std::vector<QualityActionCandidate> out;

    for (const auto& resource : resources) {
        if (resource.id == 0 || resource.levels.empty()) continue;

        const double domain_ms = RenderQualityModel::domain_time_ms(timings, resource.domain);
        QualityResourceProfile profile{};
        profile.id = resource.id;
        profile.domain = resource.domain;
        profile.label = resource.label;
        profile.semantic = resource.semantic;
        profile.importance = resource.importance;
        profile.confidence = resource.confidence;
        profile.reversible = resource.reversible;
        profile.temporal_assist = resource.temporal_assist;
        profile.levels.reserve(resource.levels.size());

        for (const auto& level : resource.levels) {
            QualityLevelStep q{};
            q.retained_quality = std::clamp(level.retained_quality, 0.0, 1.0);
            q.expected_ms_gain = domain_ms * std::clamp(level.relative_cost_reduction, 0.0, 1.0);
            q.memory_freed_bytes = level.memory_freed_bytes;
            profile.levels.push_back(q);
        }

        auto candidates = QualityCandidateFactory::build(profile);
        out.insert(out.end(),
                   std::make_move_iterator(candidates.begin()),
                   std::make_move_iterator(candidates.end()));
    }

    return out;
}

} // namespace arc
