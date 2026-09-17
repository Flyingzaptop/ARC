#include "arc/adaptive_quality.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace arc;

namespace {
void test_importance_estimator() {
    ResourceImportanceSample ui{};
    ui.screen_coverage = 0.05;
    ui.visibility = 1.0;
    ui.motion_salience = 0.1;
    ui.semantic_importance = 1.0;
    ui.normalized_distance = 0.0;

    ResourceImportanceSample distant{};
    distant.screen_coverage = 0.02;
    distant.visibility = 0.4;
    distant.motion_salience = 0.0;
    distant.semantic_importance = 0.1;
    distant.normalized_distance = 1.0;

    assert(ResourceImportanceEstimator::score(ui) > ResourceImportanceEstimator::score(distant));
}

void test_bottleneck_classification() {
    FrameBudgetSample s{};
    s.frame_ms = 21.0;
    s.target_frame_ms = 16.6667;
    s.gpu_busy_fraction = 0.99;
    s.memory_bandwidth_fraction = 0.96;
    s.raster_pressure = 0.40;
    s.geometry_pressure = 0.30;
    s.lighting_pressure = 0.50;
    s.local_usage_bytes = 4ull << 30;
    s.local_budget_bytes = 6ull << 30;
    assert(FrameBottleneckAnalyzer::classify(s) == BottleneckClass::MemoryBandwidth);

    s.local_usage_bytes = 5980ull << 20;
    s.local_budget_bytes = 6000ull << 20;
    assert(FrameBottleneckAnalyzer::classify(s) == BottleneckClass::MemoryCapacity);
}

void test_shadow_is_first_class_bottleneck() {
    FrameBudgetSample s{};
    s.frame_ms = 20.0;
    s.target_frame_ms = 16.7;
    s.gpu_busy_fraction = 0.99;
    s.memory_bandwidth_fraction = 0.20;
    s.raster_pressure = 0.30;
    s.geometry_pressure = 0.30;
    s.lighting_pressure = 0.40;
    s.shadow_pressure = 0.98;
    assert(FrameBottleneckAnalyzer::classify(s) == BottleneckClass::Shadow);

    AdaptiveQualityConfig cfg{};
    cfg.minimum_gain_ms = 0.01;
    AdaptiveQualityOptimizer optimizer{cfg};
    std::vector<QualityActionCandidate> candidates{
        {101, QualityDomain::Lighting, "cheap but unrelated light action", 5.0, 0.001, 0.99, 0, true, false, 0},
        {102, QualityDomain::Raster, "cheap but unrelated raster action", 5.0, 0.001, 0.99, 0, true, false, 0},
        {103, QualityDomain::Shadow, "far shadow update", 1.0, 0.06, 0.95, 0, true, false, 0},
    };
    const auto plan = optimizer.plan_degrade(s, candidates);
    assert(plan.bottleneck == BottleneckClass::Shadow);
    assert(!plan.actions.empty());
    for (const auto& action : plan.actions) assert(action.domain == QualityDomain::Shadow);
}

void test_temporal_default_off() {
    AdaptiveQualityOptimizer optimizer{};
    FrameBudgetSample s{};
    s.frame_ms = 24.0;
    s.target_frame_ms = 16.0;
    s.gpu_busy_fraction = 1.0;
    s.raster_pressure = 0.9;

    std::vector<QualityActionCandidate> c{
        {1, QualityDomain::Temporal, "DLSS", 8.0, 0.01, 0.99, 0, true, true, 0},
        {2, QualityDomain::Raster, "reduce distant raster", 2.0, 0.10, 0.9, 0, true, false, 0},
    };
    auto p = optimizer.plan_degrade(s, c);
    assert(!p.temporal_used);
    for (const auto& action : p.actions) assert(action.domain != QualityDomain::Temporal);
}

void test_selects_low_visual_cost_mix() {
    AdaptiveQualityOptimizer optimizer{};
    FrameBudgetSample s{};
    s.frame_ms = 20.0;
    s.target_frame_ms = 16.7;
    s.gpu_busy_fraction = 0.99;
    s.lighting_pressure = 0.95;

    std::vector<QualityActionCandidate> c{
        {10, QualityDomain::Lighting, "far light update", 1.4, 0.04, 0.90, 0, true, false, 0},
        {11, QualityDomain::Shadow, "shadow resolution", 1.1, 0.08, 0.95, 64ull << 20, true, false, 0},
        {12, QualityDomain::Lighting, "near key light", 2.0, 0.80, 0.99, 0, true, false, 0},
        {13, QualityDomain::Raster, "unrelated raster", 3.0, 0.01, 0.99, 0, true, false, 0},
    };
    auto p = optimizer.plan_degrade(s, c);
    assert(p.bottleneck == BottleneckClass::Lighting);
    assert(!p.actions.empty());
    assert(p.actions.front().id == 10);
    assert(p.planned_gain_ms >= 2.5);
    for (const auto& a : p.actions) assert(a.id != 13);
}

void test_sequence_dependencies() {
    AdaptiveQualityOptimizer optimizer{};
    FrameBudgetSample s{};
    s.frame_ms = 25.0;
    s.target_frame_ms = 16.0;
    s.gpu_busy_fraction = 0.99;
    s.memory_bandwidth_fraction = 0.99;

    std::vector<QualityActionCandidate> c{
        {50, QualityDomain::Texture, "8k->4k", 0.5, 0.02, 0.95, 64ull << 20, true, false, 0},
        {50, QualityDomain::Texture, "4k->2k", 0.7, 0.03, 0.95, 16ull << 20, true, false, 1},
        {50, QualityDomain::Texture, "2k->1k", 0.8, 0.06, 0.95, 4ull << 20, true, false, 2},
    };
    auto p = optimizer.plan_degrade(s, c);
    assert(p.actions.size() >= 2);
    for (std::size_t i = 0; i < p.actions.size(); ++i) {
        assert(p.actions[i].sequence == i);
    }
}

void test_sequence_can_continue_after_active_prefix_removed() {
    AdaptiveQualityOptimizer optimizer{};
    FrameBudgetSample s{};
    s.frame_ms = 22.0;
    s.target_frame_ms = 16.0;
    s.gpu_busy_fraction = 0.99;
    s.memory_bandwidth_fraction = 0.99;

    std::vector<QualityActionCandidate> remaining{
        {77, QualityDomain::Texture, "4k->2k", 1.0, 0.03, 0.95, 32ull << 20, true, false, 1},
        {77, QualityDomain::Texture, "2k->1k", 1.0, 0.05, 0.95, 8ull << 20, true, false, 2},
    };
    const auto p = optimizer.plan_degrade(s, remaining);
    assert(!p.actions.empty());
    assert(p.actions.front().sequence == 1);
}

void test_emergency_memory_requires_real_relief_target() {
    AdaptiveQualityOptimizer optimizer{};
    FrameBudgetSample s{};
    s.frame_ms = 16.0;
    s.target_frame_ms = 16.0;
    s.gpu_busy_fraction = 0.7;
    s.local_budget_bytes = 1000;
    s.local_usage_bytes = 980;

    std::vector<QualityActionCandidate> insufficient{
        {1, QualityDomain::Texture, "small relief", 0.1, 0.01, 0.99, 20, true, false, 0},
    };
    auto p = optimizer.plan_degrade(s, insufficient);
    assert(p.shortfall);
    assert(p.planned_memory_freed_bytes == 20);

    std::vector<QualityActionCandidate> sufficient{
        {1, QualityDomain::Texture, "first relief", 0.1, 0.01, 0.99, 50, true, false, 0},
        {2, QualityDomain::Shadow, "second relief", 0.1, 0.02, 0.99, 40, true, false, 0},
    };
    p = optimizer.plan_degrade(s, sufficient);
    assert(!p.shortfall);
    assert(p.planned_memory_freed_bytes >= 80);
}

void test_restore_prefers_visible_quality() {
    AdaptiveQualityOptimizer optimizer{};
    FrameBudgetSample s{};
    s.frame_ms = 10.0;
    s.target_frame_ms = 16.7;

    std::vector<QualityActionCandidate> active{
        {1, QualityDomain::Texture, "face texture", 1.0, 0.70, 0.99, 32ull << 20, true, false, 0},
        {2, QualityDomain::Shadow, "far shadow", 0.4, 0.15, 0.99, 16ull << 20, true, false, 0},
    };
    auto p = optimizer.plan_restore(s, active);
    assert(!p.actions.empty());
    assert(p.actions.front().id == 1);
}

void test_temporal_can_be_explicitly_enabled() {
    AdaptiveQualityConfig cfg{};
    cfg.allow_temporal_assist = true;
    AdaptiveQualityOptimizer optimizer{cfg};
    FrameBudgetSample s{};
    s.frame_ms = 30.0;
    s.target_frame_ms = 16.7;
    s.gpu_busy_fraction = 1.0;

    std::vector<QualityActionCandidate> c{
        {1, QualityDomain::Temporal, "optional upscale", 7.0, 0.01, 0.99, 0, true, true, 0},
        {2, QualityDomain::Raster, "raster", 1.0, 0.10, 0.90, 0, true, false, 0},
    };
    auto p = optimizer.plan_degrade(s, c);
    assert(p.temporal_used);
}
} // namespace

int main() {
    test_importance_estimator();
    test_bottleneck_classification();
    test_shadow_is_first_class_bottleneck();
    test_temporal_default_off();
    test_selects_low_visual_cost_mix();
    test_sequence_dependencies();
    test_sequence_can_continue_after_active_prefix_removed();
    test_emergency_memory_requires_real_relief_target();
    test_restore_prefers_visible_quality();
    test_temporal_can_be_explicitly_enabled();
    std::cout << "adaptive-quality-tests: PASS\n";
    return 0;
}
