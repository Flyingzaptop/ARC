#include "arc/render_quality_model.hpp"

#include <cassert>
#include <cmath>
#include <vector>

int main() {
    using namespace arc;

    {
        RenderPassTimings t{};
        t.texture_ms = 2.0;
        t.geometry_ms = 2.1;
        t.raster_ms = 1.9;
        t.lighting_ms = 2.0;
        t.shadow_ms = 2.0;
        t.total_ms = 10.0;
        const auto sample = RenderQualityModel::make_frame_sample(t, 8.0, 4ull << 30, 6ull << 30);
        // Balanced multi-domain pressure should remain a generic GPU bottleneck,
        // allowing utility arbitration across native domains.
        assert(FrameBottleneckAnalyzer::classify(sample) == BottleneckClass::UnknownGpu);
    }

    {
        RenderPassTimings t{};
        t.texture_ms = 1.0;
        t.geometry_ms = 1.0;
        t.raster_ms = 1.0;
        t.lighting_ms = 6.0;
        t.shadow_ms = 1.0;
        t.total_ms = 10.0;
        const auto sample = RenderQualityModel::make_frame_sample(t, 8.0, 1ull << 30, 6ull << 30);
        assert(FrameBottleneckAnalyzer::classify(sample) == BottleneckClass::Lighting);
    }

    {
        RenderPassTimings t{};
        t.texture_ms = 4.0;
        t.geometry_ms = 2.0;
        t.raster_ms = 2.0;
        t.lighting_ms = 2.0;
        t.shadow_ms = 2.0;
        t.total_ms = 12.0;
        const std::vector<RenderQualityStep> steps{
            {10, QualityDomain::Texture, "texture step", 0.25, 0.05, 0.9, 64ull << 20, true, false, 0},
            {20, QualityDomain::Geometry, "geometry step", 0.50, 0.04, 0.9, 0, true, false, 0},
            {90, QualityDomain::Temporal, "temporal step", 0.50, 0.01, 1.0, 0, true, true, 0},
        };
        const auto candidates = RenderQualityModel::build_candidates(t, steps);
        assert(candidates.size() == 3);
        assert(std::abs(candidates[0].expected_ms_gain - 1.0) < 1e-9);
        assert(std::abs(candidates[1].expected_ms_gain - 1.0) < 1e-9);

        AdaptiveQualityOptimizer optimizer{};
        const auto sample = RenderQualityModel::make_frame_sample(t, 8.0, 1ull << 30, 6ull << 30);
        const auto plan = optimizer.plan_degrade(sample, candidates);
        assert(!plan.temporal_used);
        for (const auto& action : plan.actions) assert(action.domain != QualityDomain::Temporal);
    }

    return 0;
}
