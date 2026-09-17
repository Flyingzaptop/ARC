#include "arc/live_runtime.hpp"

#include <iostream>
#include <variant>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while(false)

int main() {
    using namespace arc;

    LiveRuntimeConfig config{};
    config.residency.minimum_residency_age_epochs = 0;
    config.residency.recovery_samples = 1;
    config.residency.promotion_ceiling = 0.90; // deliberately above pressure_enter; live runtime must clamp, not discard the whole config.
    config.textures.minimum_change_age_epochs = 0;
    config.transitions.minimum_context_observations = 2;
    config.transitions.max_context_samples = 64;
    config.transitions.minimum_probability = 0.1;
    config.transitions.minimum_confidence = 0.2;
    config.minimum_transition_prefetch_confidence = 0.2;

    LiveRuntimeController runtime(config);
    CHECK(runtime.residency().config().recovery_samples == 1);
    CHECK(runtime.residency().config().promotion_ceiling == runtime.residency().config().pressure_enter);
    CHECK(runtime.register_controlled_resource({
        .id=1,.resource=100,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,
        .cost={.bytes=100,.reload_ms=1.0}}));
    CHECK(runtime.register_controlled_resource({
        .id=2,.resource=200,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,
        .cost={.bytes=100,.reload_ms=.5}}));
    CHECK(runtime.register_controlled_texture({
        .id=10,.resource=300,.safety=TextureQualitySafety::MipSafe,.full_resident_bytes=100,
        .demotion_steps={{50,.01},{25,.05}},.importance=1.0}));

    // Observation of unknown resources is allowed and trains only the sequence model.
    CHECK(runtime.note_use(999, 1, 0, 0, 0));
    CHECK(!runtime.controlled(999));

    // Controlled resources require queue/fence evidence.
    CHECK(!runtime.note_use(100, 2, 0, 0, 0));
    CHECK(runtime.note_use(100, 3, 7, 3, 3));
    CHECK(runtime.note_use(200, 4, 7, 4, 4));

    MemoryBudgetPayload pressure{};
    pressure.local_budget = 1000;
    pressure.local_usage = 900;
    runtime.update_budget(pressure);
    const auto plan = runtime.plan(100);
    CHECK(plan.pressure == PressureState::Pressure);
    CHECK(plan.pressure_relief.requested_bytes > 0);
    CHECK(!plan.pressure_relief.arbitration.actions.empty());

    const auto resolved = runtime.resolve_pressure_actions(plan.pressure_relief, 100);
    CHECK(resolved.has_value());
    CHECK(resolved->size() == plan.pressure_relief.arbitration.actions.size());
    for (const auto& action : *resolved) {
        if (const auto* residency = std::get_if<ResidencyAction>(&action)) {
            const auto object = runtime.residency().find(residency->object);
            CHECK(object.has_value());
            CHECK(runtime.controlled(object->resource));
        } else {
            const auto* texture = std::get_if<TextureQualityAction>(&action);
            CHECK(texture != nullptr);
            CHECK(runtime.controlled(texture->resource));
        }
    }

    // A plan is fail-closed if one of its subjects disappears before execution.
    const auto stale_plan = runtime.plan(101);
    CHECK(!stale_plan.pressure_relief.arbitration.actions.empty());
    const auto stale_resource = stale_plan.pressure_relief.arbitration.actions.front().candidate.resource;
    CHECK(runtime.unregister_resource(stale_resource));
    CHECK(!runtime.controlled(stale_resource));
    CHECK(!runtime.resolve_pressure_actions(stale_plan.pressure_relief, 101).has_value());

    // Transition prefetch: train A->B, evict B, then observe A under safe headroom.
    LiveRuntimeController streaming(config);
    CHECK(streaming.residency().config().recovery_samples == 1);
    CHECK(streaming.register_controlled_resource({
        .id=11,.resource=1100,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,
        .cost={.bytes=100,.reload_ms=2.0}}));
    CHECK(streaming.register_controlled_resource({
        .id=12,.resource=1200,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,
        .cost={.bytes=100,.reload_ms=2.0}}));

    std::uint64_t fence = 1;
    std::uint64_t epoch = 1;
    for (int i = 0; i < 6; ++i) {
        CHECK(streaming.note_use(1100, epoch++, 9, fence, fence)); ++fence;
        CHECK(streaming.note_use(1200, epoch++, 9, fence, fence)); ++fence;
    }

    const auto b = streaming.residency().find(12);
    CHECK(b.has_value());
    CHECK(streaming.begin_residency_action({
        ResidencyAction::Type::Evict, 12, 100, 0, 1.0, b->last_use_queue, b->last_use_fence}, epoch));
    CHECK(streaming.residency().find(12)->state == ResidencyState::Evicted);

    MemoryBudgetPayload normal{};
    normal.local_budget = 1000;
    normal.local_usage = 400;
    streaming.update_budget(normal);
    streaming.reset_sequence_context();
    CHECK(streaming.note_use(1100, epoch++, 9, fence, fence));

    const auto prefetch_plan = streaming.plan(epoch);
    CHECK(prefetch_plan.pressure == PressureState::Normal);
    CHECK(!prefetch_plan.transition_prefetch.empty());
    CHECK(prefetch_plan.transition_prefetch.front().object == 12);
    CHECK(prefetch_plan.transition_prefetch.front().type == ResidencyAction::Type::MakeResident);

    const auto metrics = streaming.metrics();
    CHECK(metrics.transition_prefetch_plans == 1);
    CHECK(metrics.transition_prefetch_actions >= 1);

    return 0;
}