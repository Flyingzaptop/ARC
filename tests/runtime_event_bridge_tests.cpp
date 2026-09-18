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
    constexpr QueueId queueA = 7;
    constexpr QueueId queueB = 8;

    CHECK(bridge.consume(make_event(EventType::CommandListCreated, CommandListPayload{.command=command})));
    CHECK(bridge.consume(make_event(EventType::ResourceUse, ResourceUsePayload{.command=command,.resource=100})));
    // Resource 999 is intentionally observed but not registered for ARC
    // mutation/control. It remains visible to the command stream without
    // participating in live residency epochs or producing a rejection.
    CHECK(bridge.consume(make_event(EventType::ResourceUse, ResourceUsePayload{.command=command,.resource=999})));
    CHECK(bridge.consume(make_event(EventType::CommandListClosed, CommandListPayload{.command=command})));

    // Same controlled resource is submitted on two independent queues. Fence
    // values are intentionally non-comparable across queues (10 vs 5).
    CHECK(bridge.consume(make_event(EventType::QueueSubmit, QueueSubmitPayload{.queue=queueA,.command=command,.submission=1})));
    CHECK(bridge.consume(make_event(EventType::QueueSubmit, QueueSubmitPayload{.queue=queueB,.command=command,.submission=2})));
    CHECK(runtime.inflight_count(100) == 2);
    CHECK(bridge.consume(make_event(EventType::FenceSignal, FencePayload{.queue=queueA,.fence=77,.value=10})));
    CHECK(bridge.consume(make_event(EventType::FenceSignal, FencePayload{.queue=queueB,.fence=88,.value=5})));

    auto object = runtime.residency().find(1);
    CHECK(object.has_value());
    CHECK(object->last_use_queue == queueB);
    CHECK(object->last_use_fence == 5);
    CHECK(object->last_completed_fence == 0);
    CHECK(bridge.logical_epoch() == 2);

    MemoryBudgetPayload pressure{};
    pressure.local_budget = 1000;
    pressure.local_usage = 900;
    CHECK(bridge.consume(make_event(EventType::MemoryBudgetSample, pressure)));
    CHECK(runtime.residency().pressure() == PressureState::Pressure);

    // Queue B completes first. The scalar last fence now looks safe, but queue A
    // is still outstanding; external in-flight accounting must reject resolve.
    bridge.note_queue_completed(queueB, 5);
    CHECK(runtime.inflight_count(100) == 1);
    auto plan = runtime.plan(100);
    CHECK(!plan.pressure_relief.arbitration.actions.empty());
    CHECK(!runtime.resolve_pressure_actions(plan.pressure_relief, 100).has_value());

    // Only after queue A also completes may the plan resolve to an executable action.
    bridge.note_queue_completed(queueA, 10);
    CHECK(runtime.inflight_count(100) == 0);
    object = runtime.residency().find(1);
    CHECK(object->last_completed_fence >= 10);
    plan = runtime.plan(101);
    CHECK(!plan.pressure_relief.arbitration.actions.empty());
    const auto resolved = runtime.resolve_pressure_actions(plan.pressure_relief, 101);
    CHECK(resolved.has_value());
    CHECK(!resolved->empty());
    CHECK(plan.pressure_relief.arbitration.actions.front().candidate.resource == 100);

    PresentPayload present{};
    present.frame = 42;
    CHECK(bridge.consume(make_event(EventType::Present, present)));
    CHECK(bridge.presentation_frame() == 42);

    CHECK(bridge.consume(make_event(EventType::ResourceDestroyed, ResourceDestroyPayload{.resource=100})));
    CHECK(!runtime.controlled(100));
    CHECK(runtime.residency().find(1) == std::nullopt);
    plan = runtime.plan(102);
    CHECK(plan.pressure_relief.arbitration.actions.empty());

    Event malformed{};
    malformed.header.type = EventType::ResourceUse;
    malformed.header.payload_bytes = 1;
    CHECK(!bridge.consume(malformed));

    const auto metrics = bridge.metrics();
    CHECK(metrics.resource_uses == 2);
    CHECK(metrics.queue_submits == 2);
    CHECK(metrics.fence_signals == 2);
    CHECK(metrics.completion_updates == 2);
    CHECK(metrics.submission_blocks == 2);
    CHECK(metrics.completion_releases == 2);
    CHECK(metrics.budget_samples == 1);
    CHECK(metrics.resources_destroyed == 1);
    CHECK(metrics.malformed_events == 1);
    CHECK(metrics.controller_rejections == 0);

    // Observation must not require fence signals just to release bookkeeping.
    // Repeated read-only submissions have no residency work to wait for.
    LiveRuntimeController observed_runtime;
    RuntimeEventBridge observed_bridge(observed_runtime, true);
    CHECK(observed_bridge.consume(make_event(EventType::CommandListCreated, CommandListPayload{.command=20})));
    CHECK(observed_bridge.consume(make_event(EventType::ResourceUse, ResourceUsePayload{.command=20,.resource=999})));
    CHECK(observed_bridge.consume(make_event(EventType::CommandListClosed, CommandListPayload{.command=20})));
    for (std::uint64_t i = 1; i <= 100000; ++i) {
        CHECK(observed_bridge.consume(make_event(EventType::QueueSubmit, QueueSubmitPayload{.queue=7,.command=20,.submission=i})));
    }
    CHECK(observed_bridge.pending_submission_count() == 0);
    CHECK(observed_bridge.metrics().submission_blocks == 0);
    CHECK(observed_bridge.metrics().controller_rejections == 0);

    return 0;
}
