#include "arc/event_ring.hpp"
#include "arc/trace.hpp"

#include <cassert>
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
