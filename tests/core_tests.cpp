#include "arc/event_ring.hpp"
#include "arc/trace.hpp"
#include "arc/resource_graph.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>

int main() {
    arc::EventRing ring(2);
    arc::Event first{};
    first.header.sequence = 1;
    arc::Event second{};
    second.header.sequence = 2;
    arc::Event third{};
    third.header.sequence = 3;
    assert(ring.try_emit(first));
    assert(ring.try_emit(second));
    assert(!ring.try_emit(third));
    assert(ring.dropped_events() == 1);
    arc::Event out{};
    assert(ring.try_pop(out) && out.header.sequence == 1);
    assert(ring.try_emit(third));
    assert(ring.try_pop(out) && out.header.sequence == 2);
    assert(ring.try_pop(out) && out.header.sequence == 3);
    assert(!ring.try_pop(out));

    arc::ResourceGraph graph;
    arc::ResourceCreatePayload create_payload{
        .resource = 42, .allocation_bytes = 4096, .width = 64, .height = 64,
        .mip_levels = 1, .kind = arc::ResourceKind::Texture2D,
    };
    arc::Event create{};
    create.header.type = arc::EventType::ResourceCreated;
    create.header.timestamp_ns = 10;
    create.header.payload_bytes = sizeof(create_payload);
    std::memcpy(create.payload.data(), &create_payload, sizeof(create_payload));
    graph.consume(create);
    assert(graph.resource_count() == 1 && graph.live_allocation_bytes() == 4096);
    arc::ResourceDestroyPayload destroy_payload{.resource = 42};
    arc::Event destroy{};
    destroy.header.type = arc::EventType::ResourceDestroyed;
    destroy.header.timestamp_ns = 20;
    destroy.header.payload_bytes = sizeof(destroy_payload);
    std::memcpy(destroy.payload.data(), &destroy_payload, sizeof(destroy_payload));
    graph.consume(destroy);
    assert(graph.live_allocation_bytes() == 0);
    assert(graph.find(42)->destroy_timestamp_ns == 20);

    const auto trace_path = std::filesystem::temp_directory_path() / "arc_core_trace_test.arcbin";
    {
        arc::TraceWriter writer(trace_path);
        assert(writer.append({&first, 1}));
        assert(writer.append({&second, 1}));
    }
    {
        std::ofstream partial(trace_path, std::ios::binary | std::ios::app);
        partial.write("incomplete", 10);
    }
    const auto recovered = arc::TraceReader::read_recoverable(trace_path);
    assert(recovered.size() == 2);
    assert(recovered[0].header.sequence == 1);
    assert(recovered[1].header.sequence == 2);
    std::filesystem::remove(trace_path);
}
