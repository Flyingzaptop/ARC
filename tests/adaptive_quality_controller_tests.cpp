#include "arc/adaptive_quality_controller.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace arc;

int main() {
    AdaptiveQualityControllerConfig cfg{};
    cfg.overload_samples_required = 2;
    cfg.headroom_samples_required = 2;
    cfg.settle_samples_after_change = 1;
    AdaptiveQualityController controller{cfg};

    FrameBudgetSample overloaded{};
    overloaded.frame_ms = 22.0;
    overloaded.target_frame_ms = 16.7;
    overloaded.gpu_busy_fraction = 0.99;
    overloaded.lighting_pressure = 0.95;

    std::vector<QualityActionCandidate> candidates{
        {1, QualityDomain::Lighting, "far light update", 2.0, 0.05, 0.95, 0, true, false, 0},
        {2, QualityDomain::Temporal, "DLSS", 8.0, 0.01, 1.0, 0, true, true, 0},
    };

    auto first = controller.tick(overloaded, candidates);
    assert(first.kind == QualityDecisionKind::None);
    auto second = controller.tick(overloaded, candidates);
    assert(second.kind == QualityDecisionKind::Degrade);
    assert(!second.plan.actions.empty());
    for (const auto& a : second.plan.actions) assert(a.domain != QualityDomain::Temporal);

    const auto action = second.plan.actions.front();
    controller.note_action_applied(action, QualityDecisionKind::Degrade, 22.0, 19.0, true);
    assert(controller.active_actions().size() == 1);

    auto settling = controller.tick(overloaded, candidates);
    assert(settling.kind == QualityDecisionKind::None);

    FrameBudgetSample headroom{};
    headroom.frame_ms = 10.0;
    headroom.target_frame_ms = 16.7;
    auto h1 = controller.tick(headroom, candidates);
    assert(h1.kind == QualityDecisionKind::None);
    auto h2 = controller.tick(headroom, candidates);
    assert(h2.kind == QualityDecisionKind::Restore);
    assert(!h2.plan.actions.empty());

    controller.note_action_applied(h2.plan.actions.front(), QualityDecisionKind::Restore, 10.0, 12.0, true);
    assert(controller.active_actions().empty());

    auto stats = controller.effects().find(action);
    assert(stats.has_value());
    assert(stats->mean_gain_ms > 2.9 && stats->mean_gain_ms < 3.1);

    std::cout << "adaptive-quality-controller-tests: PASS\n";
    return 0;
}
