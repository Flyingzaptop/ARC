#include "arc/render_candidate_catalog.hpp"

#include <cassert>
#include <cmath>
#include <vector>

int main() {
    using namespace arc;

    RenderPassTimings timings{};
    timings.texture_ms = 4.0;
    timings.geometry_ms = 3.0;
    timings.raster_ms = 2.0;
    timings.lighting_ms = 3.0;
    timings.shadow_ms = 2.0;
    timings.total_ms = 14.0;

    RenderResourceProfile far_wall{};
    far_wall.id = 100;
    far_wall.domain = QualityDomain::Texture;
    far_wall.label = "far environment texture";
    far_wall.semantic = QualitySemanticClass::Environment;
    far_wall.importance = {0.08, 0.75, 0.10, 0.20, 0.90};
    far_wall.confidence = 0.95;
    far_wall.levels = {{0.50, 0.25, 64ull << 20}};

    RenderResourceProfile weapon{};
    weapon.id = 200;
    weapon.domain = QualityDomain::Texture;
    weapon.label = "player weapon texture";
    weapon.semantic = QualitySemanticClass::PlayerWeapon;
    weapon.importance = {0.35, 1.0, 0.80, 1.0, 0.05};
    weapon.confidence = 0.95;
    weapon.levels = {{0.50, 0.25, 64ull << 20}};

    const auto candidates = RenderCandidateCatalog::build(timings, {far_wall, weapon});
    assert(candidates.size() == 2);
    assert(std::abs(candidates[0].expected_ms_gain - 1.0) < 1e-9);
    assert(std::abs(candidates[1].expected_ms_gain - 1.0) < 1e-9);
    // Same physical saving, but the foreground weapon must be much more
    // expensive to degrade than a distant environment surface.
    assert(candidates[0].visual_cost < candidates[1].visual_cost);

    FrameBudgetSample sample{};
    sample.frame_ms = 14.0;
    sample.target_frame_ms = 13.2; // one 1 ms action is enough
    sample.gpu_busy_fraction = 0.99;
    sample.memory_bandwidth_fraction = 0.95;
    sample.local_usage_bytes = 2ull << 30;
    sample.local_budget_bytes = 6ull << 30;

    AdaptiveQualityOptimizer optimizer{};
    const auto plan = optimizer.plan_degrade(sample, candidates);
    assert(!plan.actions.empty());
    assert(plan.actions.front().id == far_wall.id);
    assert(plan.actions.front().id != weapon.id);

    RenderResourceProfile temporal{};
    temporal.id = 900;
    temporal.domain = QualityDomain::Temporal;
    temporal.label = "temporal helper";
    temporal.temporal_assist = true;
    temporal.importance = {};
    temporal.levels = {{0.5, 0.8, 0}};
    const auto with_temporal = RenderCandidateCatalog::build(timings, {far_wall, temporal});
    const auto plan_no_temporal = optimizer.plan_degrade(sample, with_temporal);
    assert(!plan_no_temporal.temporal_used);
    for (const auto& action : plan_no_temporal.actions) {
        assert(action.domain != QualityDomain::Temporal);
    }

    return 0;
}
