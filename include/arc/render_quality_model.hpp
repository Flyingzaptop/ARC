#pragma once

#include "arc/adaptive_quality.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace arc {

struct RenderPassTimings {
    double texture_ms{};
    double geometry_ms{};
    double raster_ms{};
    double lighting_ms{};
    double shadow_ms{};
    double total_ms{};
};

struct RenderQualityStep {
    std::uint64_t id{};
    QualityDomain domain{QualityDomain::Texture};
    std::string label{};
    double relative_cost_reduction{}; // expected fraction of the measured domain cost removed
    double visual_cost{};
    double confidence{0.8};
    std::uint64_t memory_freed_bytes{};
    bool reversible{true};
    bool temporal_assist{};
    std::uint32_t sequence{};
};

class RenderQualityModel final {
public:
    [[nodiscard]] static double domain_time_ms(
        const RenderPassTimings& timings,
        QualityDomain domain) noexcept;

    // Converts measured per-domain GPU timings into the common ARC frame-budget
    // vocabulary. Balanced mixed workloads intentionally remain UnknownGpu so
    // the global utility optimizer may compare several native quality domains.
    [[nodiscard]] static FrameBudgetSample make_frame_sample(
        const RenderPassTimings& timings,
        double target_frame_ms,
        std::uint64_t local_usage_bytes,
        std::uint64_t local_budget_bytes,
        double gpu_busy_fraction = 0.99) noexcept;

    [[nodiscard]] static std::vector<QualityActionCandidate> build_candidates(
        const RenderPassTimings& timings,
        const std::vector<RenderQualityStep>& steps);
};

} // namespace arc
