#include "arc/memory_planner.hpp"

#include <cstdint>
#include <iostream>
#include <unordered_map>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while(false)

namespace {

arc::TextureQualityAction demotion_from_candidate(
    const arc::TextureQualityGovernor& textures,
    const arc::MemoryActionCandidate& candidate) {
    const auto object = textures.find(candidate.subject);
    if (!object || object->current_level >= object->demotion_steps.size()) return {};
    const auto level = object->current_level;
    const auto& step = object->demotion_steps[level];
    return {
        arc::TextureQualityAction::Type::Demote,
        object->id,
        object->resource,
        level,
        level + 1,
        step.bytes_freed,
        -(step.quality_loss * object->importance),
        0.0,
    };
}

arc::TextureQualityAction promotion_from_candidate(
    const arc::TextureQualityGovernor& textures,
    const arc::MemoryRestoreCandidate& candidate) {
    const auto object = textures.find(candidate.subject);
    if (!object || object->current_level == 0) return {};
    const auto level = object->current_level;
    const auto& step = object->demotion_steps[level - 1];
    return {
        arc::TextureQualityAction::Type::Promote,
        object->id,
        object->resource,
        level,
        level - 1,
        step.bytes_freed,
        step.quality_loss * object->importance,
        0.0,
    };
}

}  // namespace

int main() {
    using namespace arc;

    ResidencyPolicyConfig residency_config{};
    residency_config.minimum_residency_age_epochs = 1;
    residency_config.prefetch_horizon_epochs = 8;
    residency_config.eviction_prediction_guard_epochs = 16;
    residency_config.recovery_samples = 1;
    ResidencyGovernor residency(residency_config);

    // One periodic resource is far enough from its next use to be evictable at
    // epoch 100, then becomes a useful prefetch candidate near epoch 143.
    CHECK(residency.register_object({.id=1,.resource=101,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=80,.reload_ms=1.5}}));
    for (std::uint64_t epoch : {1ULL,51ULL}) CHECK(residency.note_use(1, epoch, epoch));
    // Cold resources provide cheap whole-resource alternatives.
    for (ResidencyId id = 2; id <= 5; ++id) {
        CHECK(residency.register_object({.id=id,.resource=100+id,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=80,.reload_ms=.2 * static_cast<double>(id)}}));
        CHECK(residency.note_use(id, 1, 1));
    }

    TextureQualityPolicyConfig texture_config{};
    texture_config.minimum_change_age_epochs = 0;
    texture_config.max_demotions_per_plan = 16;
    texture_config.max_promotions_per_plan = 16;
    TextureQualityGovernor textures(texture_config);
    CHECK(textures.register_texture({.id=10,.resource=210,.safety=TextureQualitySafety::MipSafe,.full_resident_bytes=180,.demotion_steps={{80,.01},{40,.08}},.importance=1.0}));
    CHECK(textures.register_texture({.id=11,.resource=211,.safety=TextureQualitySafety::MipSafe,.full_resident_bytes=180,.demotion_steps={{80,.03},{40,.12}},.importance=.6}));
    CHECK(textures.register_texture({.id=12,.resource=212,.safety=TextureQualitySafety::Pinned,.full_resident_bytes=180,.demotion_steps={{80,.001}},.importance=.1}));

    GlobalMemoryPlannerConfig planner_config{};
    planner_config.arbiter.quality_weight = 1.0;
    planner_config.arbiter.latency_weight = 1.0;
    planner_config.arbiter.uncertainty_weight = .05;
    GlobalMemoryPlanner planner(planner_config);

    // Pressure: 950/1000 enters Pressure and requests 170 bytes to reach 78%.
    residency.update_budget(1000, 950);
    CHECK(residency.pressure() == PressureState::Pressure);
    CHECK(residency.bytes_to_free() == 170);
    const auto relief = planner.plan_pressure_relief(residency, textures, 100);
    CHECK(!relief.arbitration.shortfall);
    CHECK(relief.arbitration.planned_bytes >= 170);

    std::uint64_t simulated_freed{};
    std::unordered_map<ResourceId, bool> whole_evicted;
    std::unordered_map<ResourceId, bool> texture_demoted;
    for (const auto& planned : relief.arbitration.actions) {
        const auto& action = planned.candidate;
        simulated_freed += action.bytes_freed;
        if (action.kind == MemoryActionKind::EvictResource) {
            whole_evicted[action.resource] = true;
            const auto object = residency.find(action.subject);
            CHECK(object && object->state == ResidencyState::Resident);
            CHECK(residency.transition(action.subject, ResidencyState::Resident, ResidencyState::Evicted));
            residency.record_eviction(action.subject, false, 100);
        } else {
            texture_demoted[action.resource] = true;
            const auto quality_action = demotion_from_candidate(textures, action);
            CHECK(quality_action.texture != 0);
            CHECK(quality_action.bytes_delta == action.bytes_freed);
            CHECK(textures.apply(quality_action, 100));
        }
    }
    CHECK(simulated_freed == relief.arbitration.planned_bytes);
    for (const auto& [resource, evicted] : whole_evicted) {
        if (evicted) CHECK(!texture_demoted[resource]);
    }
    CHECK(textures.find(12)->current_level == 0); // pinned texture never changes.

    // Recover to Normal and expose a fixed 160-byte safe headroom window.
    residency.update_budget(1000, 650);
    CHECK(residency.pressure() == PressureState::Normal);
    const auto restore = planner.plan_headroom_restore(residency, textures, 143, 160);
    CHECK(restore.arbitration.planned_bytes <= 160);

    std::uint64_t simulated_restored{};
    for (const auto& planned : restore.arbitration.actions) {
        const auto& action = planned.candidate;
        simulated_restored += action.bytes_cost;
        if (action.kind == MemoryRestoreKind::MakeResident) {
            const auto object = residency.find(action.subject);
            CHECK(object && object->state == ResidencyState::Evicted);
            CHECK(residency.transition(action.subject, ResidencyState::Evicted, ResidencyState::PendingResident));
            CHECK(residency.transition(action.subject, ResidencyState::PendingResident, ResidencyState::Resident));
            residency.record_resident(action.subject, false, 143);
        } else {
            const auto quality_action = promotion_from_candidate(textures, action);
            CHECK(quality_action.texture != 0);
            CHECK(quality_action.bytes_delta == action.bytes_cost);
            CHECK(textures.apply(quality_action, 143));
        }
    }
    CHECK(simulated_restored == restore.arbitration.planned_bytes);
    CHECK(simulated_restored <= 160);

    // Repeated pressure/recovery cycles must keep every state valid and never
    // let restoration exceed the provided headroom.
    for (std::uint64_t cycle = 0; cycle < 32; ++cycle) {
        const auto pressure_epoch = 300 + cycle * 20;
        residency.update_budget(1000, 930);
        const auto cycle_relief = planner.plan_pressure_relief(residency, textures, pressure_epoch);
        for (const auto& planned : cycle_relief.arbitration.actions) {
            const auto& action = planned.candidate;
            if (action.kind == MemoryActionKind::EvictResource) {
                const auto object = residency.find(action.subject);
                CHECK(object && object->state == ResidencyState::Resident);
                CHECK(residency.transition(action.subject, ResidencyState::Resident, ResidencyState::Evicted));
                residency.record_eviction(action.subject, false, pressure_epoch);
            } else {
                const auto quality_action = demotion_from_candidate(textures, action);
                CHECK(quality_action.texture != 0);
                CHECK(textures.apply(quality_action, pressure_epoch));
            }
        }

        residency.update_budget(1000, 600);
        const auto cycle_restore = planner.plan_headroom_restore(residency, textures, pressure_epoch + 10, 120);
        CHECK(cycle_restore.arbitration.planned_bytes <= 120);
        for (const auto& planned : cycle_restore.arbitration.actions) {
            const auto& action = planned.candidate;
            if (action.kind == MemoryRestoreKind::MakeResident) {
                const auto object = residency.find(action.subject);
                CHECK(object && object->state == ResidencyState::Evicted);
                CHECK(residency.transition(action.subject, ResidencyState::Evicted, ResidencyState::PendingResident));
                CHECK(residency.transition(action.subject, ResidencyState::PendingResident, ResidencyState::Resident));
                residency.record_resident(action.subject, false, pressure_epoch + 10);
            } else {
                const auto quality_action = promotion_from_candidate(textures, action);
                CHECK(quality_action.texture != 0);
                CHECK(textures.apply(quality_action, pressure_epoch + 10));
            }
        }
    }

    CHECK(textures.find(12)->current_level == 0);
    return 0;
}
