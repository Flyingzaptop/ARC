#include "arc/runtime_event_bridge.hpp"

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

int main() {
    using namespace arc;

    LiveRuntimeConfig config{};
    config.residency.minimum_residency_age_epochs = 0;
    config.residency.recovery_samples = 1;
    LiveRuntimeController runtime(config);
    CHECK(runtime.register_controlled_resource({
        .id=1,.resource=100,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,
        .cost={.bytes=256,.reload_ms=.5}}));

    RuntimeEventBridge bridge(runtime);
    constexpr CommandId command = 10;
    constexpr QueueId queue = 7;

    CHECK(bridge.consume(make_event(EventType::CommandListCreated, CommandListPayload{.command=command})));
    CHECK(bridge.consume(make_event(EventType::ResourceUse, ResourceUsePayload{.command=command,.resource=100})));
    CHECK(bridge.consume(make_event(EventType::ResourceUse, ResourceUsePayload{.command=command,.resource=999})));
    CHECK(bridge.consume(make_event(EventType::CommandListClosed, CommandListPayload{.command=command})));
    CHECK(bridge.consume(make_event(EventType::QueueSubmit, QueueSubmitPayload{.queue=queue,.command=command,.submission=1})));
    CHECK(bridge.consume(make_event(EventType::FenceSignal, FencePayload{.queue=queue,.fence=77,.value=10})));

    auto object = runtime.residency().find(1);
    CHECK(object.has_value());
    CHECK(object->last_use_queue == queue);
    CHECK(object->last_use_fence == 10);
    CHECK(object->last_completed_fence == 0);
    CHECK(bridge.logical_epoch() == 2); // controlled + observed-only resource

    MemoryBudgetPayload pressure{};
    pressure.local_budget = 1000;
    pressure.local_usage = 900;
    CHECK(bridge.consume(make_event(EventType::MemoryBudgetSample, pressure)));
    CHECK(runtime.residency().pressure() == PressureState::Pressure);

    // Signal alone must not make the object evictable because the real GPU fence has not completed.
    auto plan = runtime.plan(100);
    CHECK(plan.pressure_relief.arbitration.actions.empty());

    bridge.note_queue_completed(queue, 10);
    object = runtime.residency().find(1);
    CHECK(object->last_completed_fence == 10);
    plan = runtime.plan(101);
    CHECK(!plan.pressure_relief.arbitration.actions.empty());
    CHECK(plan.pressure_relief.arbitration.actions.front().candidate.resource == 100);

    PresentPayload present{};
    present.frame = 42;
    CHECK(bridge.consume(make_event(EventType::Present, present)));
    CHECK(bridge.presentation_frame() == 42);

    // Destroy removes the resource from the mutation surface immediately.
    CHECK(bridge.consume(make_event(EventType::ResourceDestroyed, ResourceDestroyPayload{.resource=100})));
    CHECK(!runtime.controlled(100));
    CHECK(runtime.residency().find(1) == std::nullopt);
    plan = runtime.plan(102);
    CHECK(plan.pressure_relief.arbitration.actions.empty());

    // Malformed events fail closed and are counted.
    Event malformed{};
    malformed.header.type = EventType::ResourceUse;
    malformed.header.payload_bytes = 1;
    CHECK(!bridge.consume(malformed));

    const auto metrics = bridge.metrics();
    CHECK(metrics.resource_uses == 2);
    CHECK(metrics.queue_submits == 1);
    CHECK(metrics.fence_signals == 1);
    CHECK(metrics.completion_updates == 1);
    CHECK(metrics.budget_samples == 1);
    CHECK(metrics.resources_destroyed == 1);
    CHECK(metrics.malformed_events == 1);
    CHECK(metrics.controller_rejections == 0);

    return 0;
}
