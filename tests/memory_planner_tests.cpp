#include "arc/memory_planner.hpp"

#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while(false)

int main() {
    using namespace arc;

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

    bool saw_texture = false;
    bool saw_residency = false;
    bool resource200_evicted = false;
    bool resource200_demoted = false;
    for (const auto& action : plan.arbitration.actions) {
        saw_texture |= action.candidate.kind == MemoryActionKind::DemoteTexture;
        saw_residency |= action.candidate.kind == MemoryActionKind::EvictResource;
        if (action.candidate.resource == 200 && action.candidate.kind == MemoryActionKind::EvictResource) resource200_evicted = true;
        if (action.candidate.resource == 200 && action.candidate.kind == MemoryActionKind::DemoteTexture) resource200_demoted = true;
    }
    CHECK(saw_texture || saw_residency);
    CHECK(!(resource200_evicted && resource200_demoted));

    // No pressure means no global action even when texture demotions are available.
    residency.update_budget(1000, 500);
    const auto idle = planner.plan_pressure_relief(residency, textures, 200);
    CHECK(idle.requested_bytes == 0);
    CHECK(idle.arbitration.actions.empty());

    return 0;
}
