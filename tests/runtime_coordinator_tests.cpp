#include "arc/runtime_coordinator.hpp"

#include <functional>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while(false)

namespace {

struct MockBackend final : arc::RuntimeMutationBackend {
    arc::RuntimeBackendStatus residency_status{arc::RuntimeBackendStatus::Success};
    arc::RuntimeBackendStatus texture_status{arc::RuntimeBackendStatus::Success};
    std::uint64_t evicts{};
    std::uint64_t make_residents{};
    std::uint64_t demotions{};
    std::uint64_t promotions{};
    std::function<void()> on_evict{};

    arc::RuntimeBackendStatus evict(arc::ResourceId, const arc::ResidencyAction&) noexcept override {
        ++evicts;
        if (on_evict) on_evict();
        return residency_status;
    }
    arc::RuntimeBackendStatus make_resident(arc::ResourceId, const arc::ResidencyAction&) noexcept override {
        ++make_residents;
        return residency_status;
    }
    arc::RuntimeBackendStatus demote_texture(arc::ResourceId, const arc::TextureQualityAction&) noexcept override {
        ++demotions;
        return texture_status;
    }
    arc::RuntimeBackendStatus promote_texture(arc::ResourceId, const arc::TextureQualityAction&) noexcept override {
        ++promotions;
        return texture_status;
    }

    [[nodiscard]] std::uint64_t calls() const noexcept {
        return evicts + make_residents + demotions + promotions;
    }
};

arc::LiveRuntimeConfig runtime_config() {
    arc::LiveRuntimeConfig config{};
    config.residency.minimum_residency_age_epochs = 0;
    config.residency.recovery_samples = 1;
    config.residency.promotion_ceiling = 0.78;
    config.textures.minimum_change_age_epochs = 0;
    return config;
}

bool register_fixture(arc::LiveRuntimeController& runtime) {
    return runtime.register_controlled_resource({
        .id=1,.resource=101,.state=arc::ResidencyState::Resident,.safety=arc::ResidencySafety::ControlledSafe,
        .cost={.bytes=200,.reload_ms=.2}}) &&
        runtime.register_controlled_resource({
            .id=2,.resource=102,.state=arc::ResidencyState::Resident,.safety=arc::ResidencySafety::ControlledSafe,
            .cost={.bytes=200,.reload_ms=.5}}) &&
        runtime.register_controlled_texture({
            .id=10,.resource=110,.safety=arc::TextureQualitySafety::MipSafe,.full_resident_bytes=200,
            .demotion_steps={{100,.02},{50,.08}},.importance=1.0});
}

void make_fence_safe(arc::LiveRuntimeController& runtime) {
    (void)runtime.note_use(101, 1, 7, 1, 1);
    (void)runtime.note_use(102, 1, 7, 2, 2);
}

arc::MemoryBudgetPayload pressure_budget() {
    arc::MemoryBudgetPayload value{};
    value.local_budget = 1000;
    value.local_usage = 900;
    return value;
}

}  // namespace

int main() {
    using namespace arc;

    // Explicit modes: observe -> plan -> controlled.
    LiveRuntimeController runtime(runtime_config());
    CHECK(register_fixture(runtime));
    make_fence_safe(runtime);
    runtime.update_budget(pressure_budget());

    MockBackend backend;
    RuntimeCoordinatorConfig coordinator_config{};
    coordinator_config.mode = RuntimeMode::ObserveOnly;
    coordinator_config.max_budget_age_ticks = 2;
    coordinator_config.max_consecutive_failures = 2;
    RuntimeCoordinator coordinator(runtime, &backend, coordinator_config);

    const auto observed = coordinator.tick(2);
    CHECK(observed.status == RuntimeTickStatus::ObservedOnly);
    CHECK(backend.calls() == 0);

    coordinator.set_mode(RuntimeMode::PlanOnly);
    const auto planned = coordinator.tick(3);
    CHECK(planned.status == RuntimeTickStatus::PlannedOnly);
    CHECK(planned.resolved_actions > 0);
    CHECK(backend.calls() == 0);

    coordinator.set_mode(RuntimeMode::Controlled);
    runtime.update_budget(pressure_budget());
    const auto controlled = coordinator.tick(4);
    CHECK(controlled.status == RuntimeTickStatus::Executed);
    CHECK(controlled.executed_actions > 0);
    CHECK(backend.calls() > 0);

    const auto calls_after_controlled = backend.calls();
    const auto stale = coordinator.tick(10);
    CHECK(stale.status == RuntimeTickStatus::BudgetStale);
    CHECK(backend.calls() == calls_after_controlled);
    CHECK(coordinator.metrics().stale_budget_blocks == 1);

    // Controlled with no backend automatically degrades to PlanOnly.
    LiveRuntimeController no_backend_runtime(runtime_config());
    CHECK(register_fixture(no_backend_runtime));
    make_fence_safe(no_backend_runtime);
    no_backend_runtime.update_budget(pressure_budget());
    RuntimeCoordinator no_backend(no_backend_runtime, nullptr, {.mode=RuntimeMode::Controlled});
    const auto no_backend_tick = no_backend.tick(2);
    CHECK(no_backend_tick.effective_mode == RuntimeMode::PlanOnly);
    CHECK(no_backend_tick.status == RuntimeTickStatus::PlannedOnly);

    // Repeated backend failures open the mutation circuit and force ObserveOnly.
    LiveRuntimeController failing_runtime(runtime_config());
    CHECK(register_fixture(failing_runtime));
    make_fence_safe(failing_runtime);
    failing_runtime.update_budget(pressure_budget());
    MockBackend failing_backend;
    failing_backend.residency_status = RuntimeBackendStatus::Failure;
    failing_backend.texture_status = RuntimeBackendStatus::Failure;
    RuntimeCoordinator failing(
        failing_runtime, &failing_backend,
        {.mode=RuntimeMode::Controlled,.max_budget_age_ticks=20,.max_consecutive_failures=2,.max_actions_per_tick=64});
    const auto first_failure = failing.tick(2);
    CHECK(first_failure.status == RuntimeTickStatus::BackendFailed);
    CHECK(!failing.circuit_open());
    const auto second_failure = failing.tick(3);
    CHECK(second_failure.status == RuntimeTickStatus::BackendFailed);
    CHECK(failing.circuit_open());
    CHECK(failing.effective_mode() == RuntimeMode::ObserveOnly);
    const auto circuit_tick = failing.tick(4);
    CHECK(circuit_tick.status == RuntimeTickStatus::CircuitOpen);
    failing.reset_circuit_breaker();
    CHECK(!failing.circuit_open());

    // If state changes after a physical Evict but before bookkeeping commits,
    // coordinator must attempt MakeResident rollback and open the circuit now.
    LiveRuntimeController race_runtime(runtime_config());
    CHECK(race_runtime.register_controlled_resource({
        .id=77,.resource=707,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,
        .cost={.bytes=400,.reload_ms=.1}}));
    CHECK(race_runtime.note_use(707, 1, 1, 5, 5));
    race_runtime.update_budget(pressure_budget());
    MockBackend race_backend;
    race_backend.on_evict = [&] { (void)race_runtime.unregister_resource(707); };
    RuntimeCoordinator race(
        race_runtime, &race_backend,
        {.mode=RuntimeMode::Controlled,.max_budget_age_ticks=20,.max_consecutive_failures=3,.max_actions_per_tick=64});
    const auto race_tick = race.tick(2);
    CHECK(race_tick.status == RuntimeTickStatus::CommitFailed);
    CHECK(race.circuit_open());
    CHECK(race_backend.evicts == 1);
    CHECK(race_backend.make_residents == 1);
    CHECK(race.metrics().rollback_attempts == 1);
    CHECK(race.metrics().commit_failures == 1);

    return 0;
}
