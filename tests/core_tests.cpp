#include "arc/event_ring.hpp"
#include "arc/trace.hpp"
#include "arc/resource_graph.hpp"
#include "arc/session.hpp"
#include "arc/footprint.hpp"

#include <cstdlib>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_set>

// Assertions must execute in Release too, and must not open a CRT dialog.
#define assert(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; std::exit(1); } } while (false)

template<class T> void feed(arc::ResourceGraph& graph, arc::EventType type, const T& payload, std::uint64_t time = 1) {
    arc::Event event{};
    event.header.type = type; event.header.timestamp_ns = time; event.header.payload_bytes = sizeof(T);
    std::memcpy(event.payload.data(), &payload, sizeof(T)); graph.consume(event);
}

int main() {
    {
        arc::IdAllocator allocator;
        std::vector<std::vector<std::uint64_t>> values(4);
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < 4; ++t) { workers.emplace_back([&, t] { for (unsigned i = 0; i < 10000; ++i) { values[t].push_back(allocator.next()); } }); }
        for (auto& worker : workers) { worker.join(); }
        std::unordered_set<std::uint64_t> unique;
        for (const auto& group : values) { for (auto id : group) { assert(id != 0); assert(unique.insert(id).second); } }
        arc::EventRing ring(31);
        std::thread producer([&] {
            for (std::uint64_t i = 1; i <= 100000; ++i) {
                arc::Event e{}; e.header.sequence = i;
                while (!ring.try_emit(e)) { std::this_thread::yield(); }
            }
        });
        for (std::uint64_t i = 1; i <= 100000; ++i) {
            arc::Event e{}; while (!ring.try_pop(e)) { std::this_thread::yield(); }
            assert(e.header.sequence == i);
        }
        producer.join();
    }
    {
        auto bc = arc::estimate_footprints(7, 5, 1, 3, 2, 1, {4, 4, 8});
        assert(bc && bc->size() == 6);
        assert((*bc)[0].logical_bytes == 32 && (*bc)[1].logical_bytes == 8);
        assert((*bc)[0].aligned_row_bytes == 256);
        auto msaa = arc::estimate_footprints(4, 4, 1, 1, 1, 4, {});
        assert(msaa && (*msaa)[0].logical_bytes == 256);
    }
    {
        arc::ResourceGraph g;
        feed(g, arc::EventType::HeapCreated, arc::HeapCreatePayload{.heap = 1, .size = 65536});
        for (auto id : {2ULL, 3ULL}) {
            feed(g, arc::EventType::ResourceCreated, arc::ResourceCreatePayload{
                .resource = id, .heap = 1, .allocation_bytes = 65536, .mip_levels = 4,
                .kind = arc::ResourceKind::Texture2D, .allocation_kind = arc::ResourceAllocationKind::Placed});
        }
        assert(g.committed_bytes() == 0 && g.live_heap_bytes() == 65536 && g.live_allocation_bytes() == 131072);
        feed(g, arc::EventType::DescriptorWritten, arc::DescriptorWrittenPayload{.descriptor = 4, .resource = 2, .type = arc::ViewType::Srv});
        g.analyze(); assert(g.find(2)->safety == arc::SafetyClass::GreenCandidate);
        feed(g, arc::EventType::DescriptorWritten, arc::DescriptorWrittenPayload{.descriptor = 4, .resource = 2, .type = arc::ViewType::Uav});
        feed(g, arc::EventType::DescriptorWritten, arc::DescriptorWrittenPayload{.descriptor = 4, .resource = 2, .type = arc::ViewType::Srv});
        g.analyze(); assert(g.find(2)->safety == arc::SafetyClass::Red);
        feed(g, arc::EventType::CommandQueueCreated, arc::QueueCreatePayload{.queue = 5});
        feed(g, arc::EventType::CommandListCreated, arc::CommandListPayload{.command = 6});
        feed(g, arc::EventType::CopyResource, arc::CopyPayload{.source = 2, .destination = 3, .command = 6});
        assert(g.find(3)->write_count == 0);
        feed(g, arc::EventType::CommandListClosed, arc::CommandListPayload{.command = 6});
        feed(g, arc::EventType::QueueSubmit, arc::QueueSubmitPayload{.queue = 5, .command = 6}, 10);
        assert(g.find(2)->read_count == 1 && g.find(2)->write_count == 0);
        assert(g.find(3)->write_count == 1 && g.find(3)->queues.contains(5));
        feed(g, arc::EventType::Present, arc::PresentPayload{.frame = 100});
        g.analyze(); assert(g.find(3)->temperature == arc::Temperature::Cold);
        assert(g.unused_for(60).size() == 2 && g.seen_on_queue(5).size() == 2);
        assert(g.with_view(arc::ViewType::Uav).size() == 1 && g.largest_textures(1).size() == 1);
        feed(g, arc::EventType::ResourceDestroyed, arc::ResourceDestroyPayload{.resource = 2}, 20);
        assert(g.alive_at(15).size() == 2 && g.alive_at(20).size() == 1);
        assert(g.errors() == 0);
    }
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

    arc::HeapCreatePayload heap_payload{.heap = 7, .size = 8192};
    arc::Event heap{};
    heap.header.type = arc::EventType::HeapCreated;
    heap.header.payload_bytes = sizeof(heap_payload);
    std::memcpy(heap.payload.data(), &heap_payload, sizeof(heap_payload));
    graph.consume(heap);
    assert(graph.live_heap_bytes() == 8192);
    arc::DescriptorWrittenPayload descriptor_payload{
        .descriptor = 9, .resource = 42, .type = arc::ViewType::Srv, .mip_count = 1, .layer_count = 1,
    };
    arc::Event descriptor{};
    descriptor.header.type = arc::EventType::DescriptorWritten;
    descriptor.header.payload_bytes = sizeof(descriptor_payload);
    std::memcpy(descriptor.payload.data(), &descriptor_payload, sizeof(descriptor_payload));
    graph.consume(descriptor);
    assert(graph.find_view(9)->description.resource == 42);
    arc::PresentPayload present_payload{.swapchain = 1, .frame = 3};
    arc::Event present{};
    present.header.type = arc::EventType::Present;
    present.header.payload_bytes = sizeof(present_payload);
    std::memcpy(present.payload.data(), &present_payload, sizeof(present_payload));
    graph.consume(present);
    assert(graph.presentation_frame() == 3);
    arc::MemoryBudgetPayload budget_payload{.local_budget = 100, .local_usage = 80};
    arc::Event budget{};
    budget.header.type = arc::EventType::MemoryBudgetSample;
    budget.header.payload_bytes = sizeof(budget_payload);
    std::memcpy(budget.payload.data(), &budget_payload, sizeof(budget_payload));
    graph.consume(budget);
    assert(graph.latest_budget()->local_usage == 80);

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
    assert(arc::TraceReader::inspect(trace_path).status == arc::TraceReader::Status::TruncatedTail);
    std::filesystem::resize_file(trace_path, std::filesystem::file_size(trace_path) - 10);
    assert(arc::TraceReader::inspect(trace_path).status == arc::TraceReader::Status::Complete);
    {
        std::fstream corrupt(trace_path, std::ios::binary | std::ios::in | std::ios::out);
        corrupt.seekp(-1, std::ios::end);
        corrupt.put('x');
    }
    assert(arc::TraceReader::inspect(trace_path).status == arc::TraceReader::Status::CorruptTail);
    assert(arc::TraceReader::inspect(trace_path).events.size() == 1);
    {
        std::ofstream incompatible(trace_path, std::ios::binary | std::ios::trunc);
        arc::TraceChunkHeader header{};
        header.schema = 999;
        incompatible.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }
    assert(arc::TraceReader::inspect(trace_path).status == arc::TraceReader::Status::SchemaMismatch);
    {
        arc::TraceWriter writer(trace_path);
        first.header.type = static_cast<arc::EventType>(60000);
        assert(writer.append({&first, 1}));
    }
    assert(arc::TraceReader::inspect(trace_path).events.size() == 1);
    std::filesystem::remove(trace_path);
    {
        arc::Session session(trace_path, 32768);
        for (std::uint64_t id = 1; id <= 10000; ++id) {
            assert(session.emit(arc::EventType::ResourceCreated, arc::ResourceCreatePayload{.resource = id, .allocation_bytes = 4096}));
            assert(session.emit(arc::EventType::ResourceDestroyed, arc::ResourceDestroyPayload{.resource = id}));
        }
        session.finish();
        assert(session.complete() && session.graph().resource_count() == 10000);
        assert(session.graph().live_allocation_bytes() == 0);
        assert(arc::TraceReader::inspect(trace_path).events.size() == 20000);
    }
    std::filesystem::remove(trace_path);
}
