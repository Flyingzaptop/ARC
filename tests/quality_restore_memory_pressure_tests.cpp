#include "arc/adaptive_quality.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace arc;

int main() {
    AdaptiveQualityConfig cfg{};
    cfg.memory_pressure_enter = 0.90;
    cfg.restoration_headroom_ms = 1.0;
    AdaptiveQualityOptimizer optimizer{cfg};

    std::vector<QualityActionCandidate> active{
        {1, QualityDomain::Texture, "restore texture", 1.0, 0.8, 0.99, 64ull << 20, true, false, 0},
    };

    FrameBudgetSample pressured{};
    pressured.frame_ms = 9.0;
    pressured.target_frame_ms = 16.7;
    pressured.local_budget_bytes = 1000;
    pressured.local_usage_bytes = 920;
    const auto blocked = optimizer.plan_restore(pressured, active);
    assert(blocked.actions.empty());

    pressured.local_usage_bytes = 700;
    const auto allowed = optimizer.plan_restore(pressured, active);
    assert(!allowed.actions.empty());
    assert(allowed.actions.front().id == 1);

    std::cout << "quality-restore-memory-pressure-tests: PASS\n";
    return 0;
}
