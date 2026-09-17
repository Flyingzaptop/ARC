#pragma once

#include "arc/quality_profile.hpp"
#include "arc/render_quality_model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace arc {

struct RenderResourceStep {
    double retained_quality{1.0};
    // Incremental fraction of the currently measured domain GPU cost expected
    // to be removed by this step.
    double relative_cost_reduction{};
    std::uint64_t memory_freed_bytes{};
};

struct RenderResourceProfile {
    std::uint64_t id{};
    QualityDomain domain{QualityDomain::Texture};
    std::string label{};
    QualitySemanticClass semantic{QualitySemanticClass::Generic};
    ResourceImportanceSample importance{};
    double confidence{0.8};
    bool reversible{true};
    bool temporal_assist{false};
    std::vector<RenderResourceStep> levels{};
};

// Bridges physical per-domain GPU timings to ARC's importance-aware quality
// profiles. The same measured saving is deliberately more expensive when the
// resource is large/visible/near/semantically protected.
class RenderCandidateCatalog final {
public:
    [[nodiscard]] static std::vector<QualityActionCandidate> build(
        const RenderPassTimings& timings,
        const std::vector<RenderResourceProfile>& resources);
};

} // namespace arc
