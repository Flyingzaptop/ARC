#include "arc/global_action_arbiter.hpp"

#include <cassert>
#include <iostream>

using namespace arc;

namespace {

QualityDecision degrade_decision(double gain_ms, double visual_cost, std::uint64_t memory_bytes = 0) {
    QualityDecision q{};
    q.kind = QualityDecisionKind::Degrade;
    q.plan.frame_deficit_ms = 2.0;
    q.plan.planned_gain_ms = gain_ms;
    q.plan.estimated_visual_cost = visual_cost;
    q.plan.planned_memory_freed_bytes = memory_bytes;
    q.plan.actions.push_back({1, QualityDomain::Raster, "quality", gain_ms, visual_cost, 0.95,
                              memory_bytes, true, false, 0});
    return q;
}

void test_frame_only_prefers_quality_and_withholds_restore() {
    GlobalActionArbiter arbiter{};
    LiveRuntimePlan memory{};
    memory.pressure = PressureState::Normal;
    memory.restore.arbitration.actions.push_back({});
    auto q = degrade_decision(2.0, 0.1);
    FrameBudgetSample f{};
    f.frame_ms = 18.0;
    f.target_frame_ms = 16.0;
    f.local_usage_bytes = 4;
    f.local_budget_bytes = 10;
    const auto d = arbiter.decide(memory, q, f);
    assert(d.execute_quality);
    assert(!d.execute_memory);
    assert(d.choice == GlobalArbitrationChoice::Quality);
}

void test_emergency_memory_preempts_non_memory_quality() {
    GlobalActionArbiter arbiter{};
    LiveRuntimePlan memory{};
    memory.pressure = PressureState::Emergency;
    memory.pressure_relief.arbitration.requested_bytes = 100;
    memory.pressure_relief.arbitration.planned_bytes = 100;
    memory.pressure_relief.arbitration.actions.push_back({});
    auto q = degrade_decision(4.0, 0.01, 0);
    FrameBudgetSample f{};
    f.frame_ms = 22.0;
    f.target_frame_ms = 16.0;
    f.local_usage_bytes = 990;
    f.local_budget_bytes = 1000;
    const auto d = arbiter.decide(memory, q, f);
    assert(d.memory_emergency);
    assert(d.execute_memory);
    assert(!d.execute_quality);
    assert(d.choice == GlobalArbitrationChoice::Memory);
}

void test_emergency_shortfall_can_combine_memory_freeing_quality() {
    GlobalActionArbiter arbiter{};
    LiveRuntimePlan memory{};
    memory.pressure = PressureState::Emergency;
    memory.pressure_relief.arbitration.requested_bytes = 200;
    memory.pressure_relief.arbitration.planned_bytes = 50;
    memory.pressure_relief.arbitration.shortfall = true;
    memory.pressure_relief.arbitration.actions.push_back({});
    auto q = degrade_decision(1.0, 0.08, 128);
    FrameBudgetSample f{};
    f.frame_ms = 20.0;
    f.target_frame_ms = 16.0;
    f.local_usage_bytes = 990;
    f.local_budget_bytes = 1000;
    const auto d = arbiter.decide(memory, q, f);
    assert(d.execute_memory);
    assert(d.execute_quality);
    assert(d.choice == GlobalArbitrationChoice::Combined);
}

void test_restore_can_coexist_with_normal_memory_restore() {
    GlobalActionArbiter arbiter{};
    LiveRuntimePlan memory{};
    memory.pressure = PressureState::Normal;
    memory.restore.arbitration.actions.push_back({});
    QualityDecision q{};
    q.kind = QualityDecisionKind::Restore;
    q.plan.estimated_visual_cost = 0.5;
    q.plan.planned_gain_ms = 0.5;
    q.plan.actions.push_back({1, QualityDomain::Lighting, "restore", 0.5, 0.5, 0.95, 0, true, false, 0});
    FrameBudgetSample f{};
    f.frame_ms = 10.0;
    f.target_frame_ms = 16.0;
    f.local_usage_bytes = 4;
    f.local_budget_bytes = 10;
    const auto d = arbiter.decide(memory, q, f);
    assert(d.execute_quality);
    assert(d.execute_memory);
    assert(d.choice == GlobalArbitrationChoice::Restore);
}

} // namespace

int main() {
    test_frame_only_prefers_quality_and_withholds_restore();
    test_emergency_memory_preempts_non_memory_quality();
    test_emergency_shortfall_can_combine_memory_freeing_quality();
    test_restore_can_coexist_with_normal_memory_restore();
    std::cout << "global-action-arbiter-tests: PASS\n";
    return 0;
}
