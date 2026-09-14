#include "arc/session.hpp"
#include <array>
#include <fstream>
namespace arc {
Session::Session(const std::filesystem::path& trace, std::size_t capacity)
    : trace_path_(trace), ring_(capacity), writer_(trace), collector_([this] { collect(); }) {}
Session::~Session() { finish(); }
void Session::finish() {
    if (stopped_) { return; }
    stopping_.store(true, std::memory_order_release);
    collector_.join(); stopped_ = true; graph_.analyze();
    try {
        std::ofstream metadata(trace_path_.string() + ".session.json");
        metadata << "{\"schema\":1,\"trace_schema\":" << kTraceSchemaVersion << ",\"complete\":" << (complete() ? "true" : "false")
            << ",\"dropped_events\":" << ring_.dropped_events() << ",\"graph_errors\":" << graph_.errors()
            << ",\"stream_model\":\"single producer; CPU timestamps\",\"byte_order\":\"little-endian\"}\n";
        if (!metadata) { io_error_ = true; }
    } catch (...) { io_error_ = true; }
}
void Session::collect() {
    try {
        std::array<Event, 256> batch{};
        std::uint64_t lastSequence{};
        for (;;) {
            std::size_t count{};
            while (count < batch.size() && ring_.try_pop(batch[count])) { graph_.consume(batch[count]); ++count; }
            if (count) { lastSequence = batch[count - 1].header.sequence; if (!writer_.append({batch.data(), count})) { io_error_ = true; } }
            else if (stopping_.load(std::memory_order_acquire)) {
                // Recheck after acquire: producer writes precede stop.
                if (!ring_.try_pop(batch[0])) {
                    if (auto dropped = ring_.dropped_events()) {
                        Event overflow{}; overflow.header.type = EventType::TraceOverflow; overflow.header.timestamp_ns = monotonic_time_ns();
                        overflow.header.sequence = lastSequence + 1; overflow.header.payload_bytes = sizeof(dropped);
                        std::memcpy(overflow.payload.data(), &dropped, sizeof(dropped)); graph_.consume(overflow);
                        if (!writer_.append({&overflow, 1})) { io_error_ = true; }
                    }
                    break;
                }
                lastSequence = batch[0].header.sequence;
                graph_.consume(batch[0]);
                if (!writer_.append({batch.data(), 1})) { io_error_ = true; }
            } else { std::this_thread::sleep_for(std::chrono::microseconds(100)); }
        }
    } catch (...) { io_error_ = true; }
}
}
