#include "arc/memory_planner.hpp"

#include <iostream>
#include <limits>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while(false)

int main() {
    using namespace arc;

    // Pressure path: combine already-safe whole-resource eviction and mip demotion candidates.
    ResidencyPolicyConfig residency_config{};
    residency_config.minimum_residency_age_epochs = 1;
    residency_config.prefetch_horizon_epochs = 4;
    residency_config.eviction_prediction_guard_epochs = 8;
    ResidencyGovernor residency(residency_config);

    CHECK(residency.register_object({.id=1,.resource=100,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=120,.reload_ms=1.5}}));
    CHECK(residency.register_object({.id=2,.resource=200,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=120,.reload_ms=.8}}));
    CHECK(residency.register_object({.id=3,.resource=300,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=120,.reload_ms=.2}}));
    CHECK(residency.note_use(1, 1, 1));
    CHECK(residency.note_use(2, 1, 1));
    CHECK(residency.note_use(3, 1, 1));
    residency.update_budget(1000, 900);
    CHECK(residency.bytes_to_free() == 120);

    TextureQualityPolicyConfig texture_config{};
    texture_config.minimum_change_age_epochs = 0;
    TextureQualityGovernor textures(texture_config);
    CHECK(textures.register_texture({
        .id=20,.resource=200,.safety=TextureQualitySafety::MipSafe,.full_resident_bytes=120,
        .demotion_steps={{60,.01},{30,.08}},.importance=1.0}));
    CHECK(textures.register_texture({
        .id=40,.resource=400,.safety=TextureQualitySafety::MipSafe,.full_resident_bytes=100,
        .demotion_steps={{70,.02}},.importance=1.0}));

    MemoryArbiterConfig arbiter_config{};
    arbiter_config.quality_weight = 1.0;
    arbiter_config.latency_weight = 1.0;
    GlobalMemoryPlanner planner({.arbiter=arbiter_config,.unknown_prediction_confidence=.5,.minimum_reload_risk_factor=.1});

    const auto plan = planner.plan_pressure_relief(residency, textures, 100);
    CHECK(plan.requested_bytes == 120);
    CHECK(plan.residency_candidate_bytes >= 120);
    CHECK(plan.texture_candidate_bytes >= 120);
    CHECK(!plan.arbitration.shortfall);
    CHECK(plan.arbitration.planned_bytes >= 120);

    bool resource200_evicted = false;
    bool resource200_demoted = false;
    for (const auto& action : plan.arbitration.actions) {
        if (action.candidate.resource == 200 && action.candidate.kind == MemoryActionKind::EvictResource) resource200_evicted = true;
        if (action.candidate.resource == 200 && action.candidate.kind == MemoryActionKind::DemoteTexture) resource200_demoted = true;
    }
    CHECK(!(resource200_evicted && resource200_demoted));

    // No pressure means no relief action even when texture demotions exist.
    residency.update_budget(1000, 500);
    const auto idle = planner.plan_pressure_relief(residency, textures, 200);
    CHECK(idle.requested_bytes == 0);
    CHECK(idle.arbitration.actions.empty());

    // Headroom path: an actually evicted periodic resource competes with previously demoted texture quality.
    ResidencyPolicyConfig restore_residency_config{};
    restore_residency_config.prefetch_horizon_epochs = 8;
    restore_residency_config.promotion_ceiling = .90;
    ResidencyGovernor restore_residency(restore_residency_config);
    CHECK(restore_residency.register_object({
        .id=10,.resource=500,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,
        .cost={.bytes=80,.reload_ms=2.0}}));
    for (std::uint64_t epoch : {10ULL,20ULL,30ULL,40ULL}) CHECK(restore_residency.note_use(10, epoch, epoch));
    CHECK(restore_residency.transition(10, ResidencyState::Resident, ResidencyState::Evicted));
    restore_residency.record_eviction(10, false, 42);
    restore_residency.update_budget(1000, 600);
    CHECK(restore_residency.pressure() == PressureState::Normal);
    CHECK(!restore_residency.plan_promotions(42).empty());

    TextureQualityPolicyConfig restore_texture_config{};
    restore_texture_config.minimum_change_age_epochs = 0;
    TextureQualityGovernor restore_textures(restore_texture_config);
    CHECK(restore_textures.register_texture({
        .id=50,.resource=600,.safety=TextureQualitySafety::MipSafe,.full_resident_bytes=120,
        .demotion_steps={{40,.10},{30,.25}},.importance=1.0}));
    const auto demotions = restore_textures.plan_demotions(70, 1);
    CHECK(demotions.size() == 2);
    for (const auto& action : demotions) CHECK(restore_textures.apply(action, 1));
    CHECK(restore_textures.find(50)->current_level == 2);

    const auto restore = planner.plan_headroom_restore(restore_residency, restore_textures, 42, 100);
    CHECK(restore.headroom_bytes == 100);
    CHECK(restore.residency_candidate_bytes == 80);
    CHECK(restore.texture_candidate_bytes >= 40);
    CHECK(restore.arbitration.planned_bytes <= 100);
    CHECK(!restore.arbitration.actions.empty());

    bool saw_restore_residency = false;
    bool saw_restore_texture = false;
    for (const auto& action : restore.arbitration.actions) {
        saw_restore_residency |= action.candidate.kind == MemoryRestoreKind::MakeResident;
        saw_restore_texture |= action.candidate.kind == MemoryRestoreKind::PromoteTexture;
    }
    CHECK(saw_restore_residency || saw_restore_texture);

    // Cold/unknown resources must not remain evicted forever. Once pressure is
    // normal and there is safe headroom they become low-priority background
    // restore candidates, but never in the same epoch in which they were evicted.
    ResidencyPolicyConfig cold_config{};
    cold_config.promotion_ceiling = .90;
    ResidencyGovernor cold(cold_config);
    CHECK(cold.register_object({
        .id=11,.resource=700,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,
        .cost={.bytes=80,.reload_ms=1.0}}));
    CHECK(cold.transition(11, ResidencyState::Resident, ResidencyState::Evicted));
    cold.record_eviction(11, false, 50);
    cold.update_budget(1000, 600);
    CHECK(cold.promotion_candidates(50).empty());
    const auto background = cold.promotion_candidates(51);
    CHECK(background.size() == 1);
    CHECK(background.front().type == ResidencyAction::Type::MakeResident);
    CHECK(background.front().object == 11);
    CHECK(background.front().predicted_use_epoch == (std::numeric_limits<std::uint64_t>::max)());
    TextureQualityGovernor no_restore_textures(restore_texture_config);
    const auto cold_restore = planner.plan_headroom_restore(cold, no_restore_textures, 51, 100);
    CHECK(cold_restore.residency_candidate_bytes == 80);
    CHECK(!cold_restore.arbitration.actions.empty());
    CHECK(cold_restore.arbitration.actions.front().candidate.kind == MemoryRestoreKind::MakeResident);

    // Restoration is strictly capped; no candidate may overfill a tiny headroom window.
    const auto tiny = planner.plan_headroom_restore(restore_residency, restore_textures, 42, 25);
    CHECK(tiny.arbitration.planned_bytes <= 25);
    CHECK(tiny.arbitration.actions.empty());

    return 0;
}
