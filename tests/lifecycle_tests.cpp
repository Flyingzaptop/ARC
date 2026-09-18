#include "arc/resource_graph.hpp"
#include "arc/runtime_event_bridge.hpp"
#include <cassert>
#include <cstring>
#include <iostream>

template<class T> arc::Event event(arc::EventType type, T p) {
    arc::Event e{}; e.header.type=type; e.header.payload_bytes=sizeof(T);
    std::memcpy(e.payload.data(), &p, sizeof(T)); return e;
}
int main() {
    using namespace arc;
    ResourceGraph graph({8,8,8});
    LiveRuntimeController runtime;
    RuntimeEventBridge bridge(runtime, true);
    auto both=[&](const Event& e) { graph.consume(e); assert(bridge.consume(e)); };
    ResourceCreatePayload r{}; r.resource=1; r.kind=ResourceKind::Buffer; r.width=4096;
    both(event(EventType::ResourceCreated,r));
    for (std::uint64_t i=1;i<=10000;++i) {
        both(event(EventType::CommandQueueCreated,QueueCreatePayload{.queue=i}));
        both(event(EventType::CommandListCreated,CommandListPayload{.command=i}));
        both(event(EventType::ResourceUse,ResourceUsePayload{.command=i,.resource=1}));
        both(event(EventType::CommandListClosed,CommandListPayload{.command=i}));
        both(event(EventType::QueueSubmit,QueueSubmitPayload{.queue=i,.command=i,.submission=i}));
        both(event(EventType::CommandListDestroyed,CommandDestroyPayload{i}));
        both(event(EventType::CommandQueueDestroyed,QueueDestroyPayload{i}));
        assert(graph.command_count()==0 && graph.queue_count()==0);
        assert(bridge.command_count()==0 && bridge.queue_state_count()==0);
        assert(graph.find(1)->queues.empty() && graph.find(1)->queue_use_counts.empty());
    }
    assert(graph.errors()==0 && graph.submissions().size()==8);
    assert(graph.workload_totals().submissions==10000);

    LiveRuntimeController controlled;
    assert(controlled.register_controlled_resource({.id=1,.resource=100,.state=ResidencyState::Resident,
        .safety=ResidencySafety::ControlledSafe,.cost={.bytes=4096,.reload_ms=.1}}));
    RuntimeEventBridge safe(controlled,true);
    assert(safe.bind_completion_fence(7,77));
    assert(safe.consume(event(EventType::CommandListCreated,CommandListPayload{.command=10})));
    assert(safe.consume(event(EventType::ResourceUse,ResourceUsePayload{.command=10,.resource=100})));
    assert(safe.consume(event(EventType::QueueSubmit,QueueSubmitPayload{.queue=7,.command=10,.submission=1})));
    assert(safe.consume(event(EventType::CommandListDestroyed,CommandDestroyPayload{10})));
    assert(controlled.inflight_count(100)==1 && !safe.can_retire_queue(7));
    assert(!safe.consume(event(EventType::CommandQueueDestroyed,QueueDestroyPayload{7})));
    assert(controlled.inflight_count(100)==1);
    assert(safe.consume(event(EventType::FenceSignal,FencePayload{.queue=7,.fence=77,.value=5})));
    assert(!safe.can_retire_queue(7));
    assert(safe.note_queue_completed(7,77,5));
    assert(safe.consume(event(EventType::CommandQueueDestroyed,QueueDestroyPayload{7})));
    assert(controlled.inflight_count(100)==0 && safe.queue_state_count()==0 && safe.command_count()==0);
    std::cout << "lifecycle-tests: PASS (10000 lifetimes; in-flight retirement rejected)\n";
}
