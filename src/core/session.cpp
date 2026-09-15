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
    collector_.join(); stopped_ = true;
    try {
        const auto graphCpuStart = current_thread_cpu_time_ns();
        auto recovered = TraceReader::inspect(trace_path_);
        if (recovered.status != TraceReader::Status::Complete) { io_error_ = true; }
        for (const auto& event : recovered.events) { graph_.consume(event); }
        graph_.analyze();
        const auto graphCpuEnd = current_thread_cpu_time_ns();
        if (graphCpuEnd >= graphCpuStart) { statistics_.offline_graph_cpu_ns = graphCpuEnd - graphCpuStart; }
        const auto writerStatistics = writer_.statistics();
        statistics_.writer_cpu_ns = writerStatistics.cpu_ns;
        statistics_.trace_payload_bytes = writerStatistics.payload_bytes;
        statistics_.trace_chunks = writerStatistics.chunks;
        statistics_.checkpoints = writerStatistics.checkpoints;
        statistics_.packing_capacity = writerStatistics.packing_capacity;
        std::ofstream metadata(trace_path_.string() + ".session.json");
        metadata << "{\"schema\":1,\"trace_schema\":" << kTraceSchemaVersion << ",\"complete\":" << (complete() ? "true" : "false")
            << ",\"dropped_events\":" << ring_.dropped_events() << ",\"graph_errors\":" << graph_.errors()
            << ",\"stream_model\":\"single producer; CPU timestamps\",\"byte_order\":\"little-endian\"}\n";
        if (!metadata) { io_error_ = true; }
    } catch (...) { io_error_ = true; }
}
void Session::collect() {
    const auto collectorCpuStart = current_thread_cpu_time_ns();
    try {
        // A 4096-event batch is 1 MiB; keep it off the collector's limited
        // Windows thread stack and allocate it once before capture begins.
        std::vector<Event> batch(4096);
        std::uint64_t lastSequence{};
        for (;;) {
            std::size_t count{};
            while (count < batch.size() && ring_.try_pop(batch[count])) { ++count; }
            if (count) { lastSequence = batch[count - 1].header.sequence; if (!writer_.append({batch.data(), count})) { io_error_ = true; } }
            else if (stopping_.load(std::memory_order_acquire)) {
                // Recheck after acquire: producer writes precede stop.
                if (!ring_.try_pop(batch[0])) {
                    if (auto dropped = ring_.dropped_events()) {
                        Event overflow{}; overflow.header.type = EventType::TraceOverflow; overflow.header.timestamp_ns = monotonic_time_ns();
                        overflow.header.sequence = lastSequence + 1; overflow.header.payload_bytes = sizeof(dropped);
                        std::memcpy(overflow.payload.data(), &dropped, sizeof(dropped));
                        if (!writer_.append({&overflow, 1})) { io_error_ = true; }
                    }
                    break;
                }
                lastSequence = batch[0].header.sequence;
                if (!writer_.append({batch.data(), 1})) { io_error_ = true; }
            } else { std::this_thread::sleep_for(std::chrono::microseconds(100)); }
        }
        if (!writer_.checkpoint()) { io_error_ = true; }
    } catch (...) { io_error_ = true; }
    const auto collectorCpuEnd = current_thread_cpu_time_ns();
    if (collectorCpuEnd >= collectorCpuStart) { statistics_.collector_cpu_ns = collectorCpuEnd - collectorCpuStart; }
}
}
