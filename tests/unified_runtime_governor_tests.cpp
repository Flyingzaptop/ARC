#include "arc/runtime_integration.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace arc;

namespace {

struct MockBackend final : RuntimeMutationBackend {
    int evictions{};
    int residents{};
    int quality_applies{};
    int quality_restores{};
    RuntimeBackendStatus quality_status{RuntimeBackendStatus::Success};

    RuntimeBackendStatus evict(ResourceId, const ResidencyAction&) noexcept override {
        ++evictions;
        return RuntimeBackendStatus::Success;
    }
    RuntimeBackendStatus make_resident(ResourceId, const ResidencyAction&) noexcept override {
        ++residents;
        return RuntimeBackendStatus::Success;
    }
    RuntimeBackendStatus demote_texture(ResourceId, const TextureQualityAction&) noexcept override {
        return RuntimeBackendStatus::Success;
    }
    RuntimeBackendStatus promote_texture(ResourceId, const TextureQualityAction&) noexcept override {
        return RuntimeBackendStatus::Success;
    }
    RuntimeBackendStatus apply_quality(const QualityActionCandidate&) noexcept override {
        ++quality_applies;
        return quality_status;
    }
    RuntimeBackendStatus restore_quality(const QualityActionCandidate&) noexcept override {
        ++quality_restores;
        return quality_status;
    }
};

QualityResourceProfile raster_profile(std::uint64_t id = 900) {
    QualityResourceProfile p{};
    p.id = id;
    p.domain = QualityDomain::Raster;
    p.label = "distant raster";
    p.semantic = QualitySemanticClass::Environment;
    p.importance.screen_coverage = 0.15;
    p.importance.visibility = 0.8;
    p.importance.semantic_importance = 0.15;
    p.importance.normalized_distance = 0.85;
    p.confidence = 0.98;
    p.reversible = true;
    p.levels = {
        {0.80, 2.0, 0},
        {0.60, 1.5, 0},
    };
    return p;
}

RuntimeIntegrationConfig fast_config() {
    RuntimeIntegrationConfig cfg{};
    cfg.coordinator.mode = RuntimeMode::Controlled;
    cfg.governor.quality.overload_samples_required = 1;
    cfg.governor.quality.headroom_samples_required = 1;
    cfg.governor.quality.settle_samples_after_change = 0;
    cfg.governor.quality.minimum_hold_samples_after_degrade = 0;
    cfg.governor.quality.frame_ewma_alpha = 1.0;
    cfg.governor.quality.optimizer.minimum_gain_ms = 0.01;
    cfg.governor.quality.optimizer.restoration_headroom_ms = 0.5;
    cfg.governor.quality.extra_restore_headroom_ms = 0.0;
    return cfg;
}

void test_live_quality_apply_measure_restore() {
    MockBackend backend;
    RuntimeIntegration runtime{&backend, fast_config()};
    assert(runtime.register_quality_profile(raster_profile()));

    FrameBudgetSample heavy{};
    heavy.frame_ms = 20.0;
    heavy.target_frame_ms = 16.0;
    heavy.gpu_busy_fraction = 1.0;
    heavy.raster_pressure = 0.95;

    const auto first = runtime.tick_adaptive(10, heavy);
    assert(first.quality.kind == QualityDecisionKind::Degrade);
    assert(first.quality_executed);
    assert(backend.quality_applies == 1);

    FrameBudgetSample easy = heavy;
    easy.frame_ms = 10.0;
    easy.raster_pressure = 0.2;
    const auto second = runtime.tick_adaptive(11, easy);
    assert(second.pending_effect_resolved);
    assert(second.quality.kind == QualityDecisionKind::Restore);
    assert(second.quality_executed);
    assert(backend.quality_restores == 1);

    const auto third = runtime.tick_adaptive(12, easy);
    assert(third.pending_effect_resolved);
    assert(runtime.governor().quality().active_actions().empty());
    const auto metrics = runtime.governor().metrics();
    assert(metrics.quality_actions_executed == 2);
    assert(metrics.pending_effects_resolved == 2);
}

void test_memory_emergency_preempts_extra_quality_mutation() {
    MockBackend backend;
    auto cfg = fast_config();
    cfg.runtime.residency.minimum_residency_age_epochs = 1;
    cfg.runtime.residency.pressure_enter = 0.85;
    cfg.runtime.residency.emergency_enter = 0.95;
    cfg.runtime.residency.pressure_target = 0.75;
    cfg.runtime.residency.emergency_target = 0.70;
    RuntimeIntegration runtime{&backend, cfg};
    assert(runtime.register_quality_profile(raster_profile(901)));

    ResidencyObject object{};
    object.id = 1;
    object.resource = 1001;
    object.state = ResidencyState::Resident;
    object.safety = ResidencySafety::ControlledSafe;
    object.cost.bytes = 400;
    object.cost.reload_ms = 0.2;
    object.last_resident_epoch = 1;
    assert(runtime.register_controlled_resource(object));

    MemoryBudgetPayload budget{};
    budget.local_budget = 1000;
    budget.local_usage = 990;
    runtime.update_budget(budget);

    FrameBudgetSample frame{};
    frame.frame_ms = 22.0;
    frame.target_frame_ms = 16.0;
    frame.gpu_busy_fraction = 1.0;
    frame.raster_pressure = 0.95;
    frame.local_budget_bytes = 1000;
    frame.local_usage_bytes = 990;

    const auto result = runtime.tick_adaptive(100, frame);
    assert(result.memory_pressure >= 0.99);
    assert(result.memory.executed_actions >= 1);
    assert(backend.evictions >= 1);
    assert(!result.quality_executed);
    assert(backend.quality_applies == 0);
}

void test_admission_is_exposed_through_runtime() {
    MockBackend backend;
    RuntimeIntegration runtime{&backend, fast_config()};

    QualityAdmissionResource r{};
    r.profile = raster_profile(902);
    r.profile.domain = QualityDomain::Texture;
    r.profile.importance.screen_coverage = 0.01;
    r.profile.importance.visibility = 0.4;
    r.profile.importance.normalized_distance = 1.0;
    r.profile.levels = {
        {0.50, 0.3, 64ull << 20},
        {0.25, 0.4, 16ull << 20},
    };
    r.requested_bytes = 96ull << 20;

    FrameBudgetSample f{};
    f.frame_ms = 25.0;
    f.target_frame_ms = 16.0;
    f.local_usage_bytes = 5900ull << 20;
    f.local_budget_bytes = 6000ull << 20;
    const auto decision = runtime.admit_quality(r, f);
    assert(decision.valid);
    assert(decision.reduced);
    assert(decision.estimated_bytes_saved != 0);
}

void test_quality_backend_failures_trip_local_circuit() {
    MockBackend backend;
    backend.quality_status = RuntimeBackendStatus::Failure;
    auto cfg = fast_config();
    cfg.governor.max_consecutive_quality_failures = 2;
    RuntimeIntegration runtime{&backend, cfg};
    assert(runtime.register_quality_profile(raster_profile(903)));

    FrameBudgetSample heavy{};
    heavy.frame_ms = 20.0;
    heavy.target_frame_ms = 16.0;
    heavy.gpu_busy_fraction = 1.0;
    heavy.raster_pressure = 0.95;

    auto a = runtime.tick_adaptive(1, heavy);
    assert(a.quality_attempted && !a.quality_executed);
    auto b = runtime.tick_adaptive(2, heavy);
    assert(b.quality_attempted && !b.quality_executed);
    assert(runtime.governor().quality_circuit_open());
    auto c = runtime.tick_adaptive(3, heavy);
    assert(!c.quality_executed);
}

} // namespace

int main() {
    test_live_quality_apply_measure_restore();
    test_memory_emergency_preempts_extra_quality_mutation();
    test_admission_is_exposed_through_runtime();
    test_quality_backend_failures_trip_local_circuit();
    std::cout << "unified-runtime-governor-tests: PASS\n";
    return 0;
}
