#include "arc/quality_admission.hpp"

#include <cassert>
#include <iostream>

using namespace arc;

namespace {
QualityAdmissionResource distant_environment() {
    QualityAdmissionResource r{};
    r.profile.id = 100;
    r.profile.domain = QualityDomain::Texture;
    r.profile.label = "distant wall texture";
    r.profile.semantic = QualitySemanticClass::Environment;
    r.profile.importance.screen_coverage = 0.03;
    r.profile.importance.visibility = 0.65;
    r.profile.importance.motion_salience = 0.0;
    r.profile.importance.semantic_importance = 0.10;
    r.profile.importance.normalized_distance = 0.95;
    r.profile.confidence = 0.98;
    r.profile.reversible = true;
    r.profile.levels = {
        {0.75, 0.20, 96ull << 20},
        {0.50, 0.28, 24ull << 20},
        {0.25, 0.32, 6ull << 20},
    };
    r.requested_bytes = 128ull << 20;
    return r;
}

void test_no_pressure_keeps_full_quality() {
    QualityAdmissionController controller{};
    auto r = distant_environment();
    FrameBudgetSample f{};
    f.frame_ms = 12.0;
    f.target_frame_ms = 16.7;
    f.local_usage_bytes = 3ull << 30;
    f.local_budget_bytes = 6ull << 30;
    const auto d = controller.decide(r, f);
    assert(d.valid);
    assert(!d.reduced);
    assert(d.admitted_level == 0);
    assert(d.retained_quality == 1.0);
}

void test_pressure_admits_reduced_distant_resource() {
    QualityAdmissionController controller{};
    auto r = distant_environment();
    FrameBudgetSample f{};
    f.frame_ms = 24.0;
    f.target_frame_ms = 16.7;
    f.local_usage_bytes = 5900ull << 20;
    f.local_budget_bytes = 6000ull << 20;
    const auto d = controller.decide(r, f);
    assert(d.valid);
    assert(d.reduced);
    assert(d.admitted_level >= 1);
    assert(!d.initial_actions.empty());
    assert(d.initial_actions.front().sequence == 0);
    assert(d.estimated_bytes_saved >= (96ull << 20));
    for (std::size_t i = 0; i < d.initial_actions.size(); ++i) {
        assert(d.initial_actions[i].sequence == i);
    }
}

void test_ui_is_semantically_protected() {
    QualityAdmissionController controller{};
    auto r = distant_environment();
    r.profile.id = 101;
    r.profile.semantic = QualitySemanticClass::Ui;
    r.profile.importance.semantic_importance = 1.0;
    r.profile.importance.visibility = 1.0;
    r.profile.importance.normalized_distance = 0.0;

    FrameBudgetSample f{};
    f.frame_ms = 30.0;
    f.target_frame_ms = 16.7;
    f.local_usage_bytes = 5990ull << 20;
    f.local_budget_bytes = 6000ull << 20;
    const auto d = controller.decide(r, f);
    assert(d.valid);
    assert(d.protected_semantic);
    assert(!d.reduced);
}

void test_temporal_profile_is_not_admitted_by_native_policy() {
    QualityAdmissionController controller{};
    auto r = distant_environment();
    r.profile.id = 102;
    r.profile.domain = QualityDomain::Temporal;
    r.profile.temporal_assist = true;
    FrameBudgetSample f{};
    f.frame_ms = 30.0;
    f.target_frame_ms = 16.7;
    const auto d = controller.decide(r, f);
    assert(!d.valid);
    assert(!d.reduced);
}
} // namespace

int main() {
    test_no_pressure_keeps_full_quality();
    test_pressure_admits_reduced_distant_resource();
    test_ui_is_semantically_protected();
    test_temporal_profile_is_not_admitted_by_native_policy();
    std::cout << "quality-admission-tests: PASS\n";
    return 0;
}
