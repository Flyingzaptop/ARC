#include "arc/runtime_integration.hpp"

#include <cstring>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while(false)

template<class T>
arc::Event make_event(arc::EventType type, const T& payload) {
    arc::Event event{};
    event.header.type = type;
    event.header.payload_bytes = sizeof(T);
    std::memcpy(event.payload.data(), &payload, sizeof(T));
    return event;
}

namespace {
struct MockBackend final : arc::RuntimeMutationBackend {
    std::uint64_t evicts{};
    std::uint64_t residents{};
    arc::RuntimeBackendStatus evict(arc::ResourceId, const arc::ResidencyAction&) noexcept override {
        ++evicts; return arc::RuntimeBackendStatus::Success;
    }
    arc::RuntimeBackendStatus make_resident(arc::ResourceId, const arc::ResidencyAction&) noexcept override {
        ++residents; return arc::RuntimeBackendStatus::Success;
    }
    arc::RuntimeBackendStatus demote_texture(arc::ResourceId, const arc::TextureQualityAction&) noexcept override {
        return arc::RuntimeBackendStatus::Success;
    }
    arc::RuntimeBackendStatus promote_texture(arc::ResourceId, const arc::TextureQualityAction&) noexcept override {
        return arc::RuntimeBackendStatus::Success;
    }
};
}

int main() {
    using namespace arc;

    MockBackend backend;
    RuntimeIntegrationConfig config{};
    config.runtime.residency.minimum_residency_age_epochs = 0;
    config.runtime.residency.recovery_samples = 1;
    config.coordinator.mode = RuntimeMode::Controlled;
    config.coordinator.max_budget_age_ticks = 20;
    config.coordinator.max_consecutive_failures = 3;
    RuntimeIntegration integration(&backend, config);

    CHECK(integration.register_controlled_resource({
        .id=1,.resource=100,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,
        .cost={.bytes=400,.reload_ms=.2}}));

    constexpr CommandId command = 10;
    constexpr QueueId queue = 7;
    constexpr std::uint64_t completion_fence = 55;
    CHECK(integration.bind_completion_fence(queue, completion_fence));
    CHECK(integration.consume(make_event(EventType::CommandListCreated, CommandListPayload{.command=command})));
    CHECK(integration.consume(make_event(EventType::ResourceUse, ResourceUsePayload{.command=command,.resource=100})));
    CHECK(integration.consume(make_event(EventType::CommandListClosed, CommandListPayload{.command=command})));
    CHECK(integration.consume(make_event(EventType::QueueSubmit, QueueSubmitPayload{.queue=queue,.command=command,.submission=1})));

    // An unrelated fence on the same queue has a completely unrelated value.
    // It must not flush the submission or make the resource evictable.
    CHECK(integration.consume(make_event(EventType::FenceSignal, FencePayload{.queue=queue,.fence=99,.value=1000000})));

    MemoryBudgetPayload pressure{};
    pressure.local_budget = 1000;
    pressure.local_usage = 900;
    CHECK(integration.consume(make_event(EventType::MemoryBudgetSample, pressure)));
    const auto wrong_fence_blocked = integration.tick(9);
    CHECK(wrong_fence_blocked.budget_fresh);
    CHECK(wrong_fence_blocked.status == RuntimeTickStatus::ResolveFailed);
    CHECK(backend.evicts == 0);
    CHECK(integration.bridge().metrics().ignored_fence_signals == 1);

    // Only the explicitly bound completion fence turns pending command uses into
    // a residency fence requirement.
    CHECK(integration.consume(make_event(EventType::FenceSignal, FencePayload{
        .queue=queue,.fence=completion_fence,.value=9})));
    const auto blocked = integration.tick(10);
    CHECK(blocked.budget_fresh);
    CHECK(blocked.status == RuntimeTickStatus::NoAction);
    CHECK(blocked.resolved_actions == 0);
    CHECK(backend.evicts == 0);

    // A completion update for a different fence identity is rejected.
    CHECK(!integration.note_queue_completed(queue, 99, 1000000));
    CHECK(backend.evicts == 0);
    CHECK(integration.note_queue_completed(queue, completion_fence, 9));
    const auto executed = integration.tick(11);
    CHECK(executed.status == RuntimeTickStatus::Executed);
    CHECK(executed.executed_actions == 1);
    CHECK(backend.evicts == 1);
    CHECK(integration.runtime().residency().find(1)->state == ResidencyState::Evicted);

    CHECK(integration.consume(make_event(EventType::ResourceDestroyed, ResourceDestroyPayload{.resource=100})));
    CHECK(!integration.runtime().controlled(100));
    CHECK(integration.unbind_completion_fence(queue));

    // Default integration is observe-only even if a backend is supplied.
    MockBackend observe_backend;
    RuntimeIntegration observe(&observe_backend, {});
    CHECK(observe.register_controlled_resource({
        .id=2,.resource=200,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,
        .cost={.bytes=400,.reload_ms=.1}}));
    CHECK(observe.runtime().note_use(200, 1, 1, 1, 1));
    observe.update_budget(pressure);
    const auto observe_tick = observe.tick(2);
    CHECK(observe_tick.status == RuntimeTickStatus::ObservedOnly);
    CHECK(observe_backend.evicts == 0);

    return 0;
}
