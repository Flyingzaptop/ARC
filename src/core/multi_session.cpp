#include "arc/multi_session.hpp"
#include <array>
#include <fstream>
namespace arc {
MultiSession::MultiSession(const std::filesystem::path& path, std::size_t producers, std::size_t capacity) : path_(path) {
    if (!producers || producers > 1024) { throw std::invalid_argument("producer count must be 1..1024"); }
    for (std::size_t i = 0; i < producers; ++i) { rings_.push_back(std::make_unique<EventRing>(capacity)); }
    collector_ = std::thread([this] { collect(); });
}
MultiSession::~MultiSession() { finish(); }
std::uint64_t MultiSession::dropped() const noexcept {
    std::uint64_t result{}; for (const auto& ring : rings_) { result += ring->dropped_events(); } return result;
}
void MultiSession::collect() {
    try {
        TraceWriter writer(path_); std::array<Event, 256> batch{};
        for (;;) {
            bool work = false;
            for (const auto& ring : rings_) {
                std::size_t count{}; while (count < batch.size() && ring->try_pop(batch[count])) { ++count; }
                if (count) { work = true; if (!writer.append({batch.data(), count})) { failed_ = true; } }
            }
            if (!work && stopping_.load(std::memory_order_acquire)) {
                // Stop is published only after all producer threads have joined.
                bool remainder = false;
                for (const auto& ring : rings_) { if (ring->try_pop(batch[0])) { remainder = true; if (!writer.append({batch.data(), 1})) { failed_ = true; } } }
                if (!remainder) { break; }
            } else if (!work) { std::this_thread::sleep_for(std::chrono::microseconds(100)); }
        }
        if (auto count = dropped()) {
            Event e{}; e.header.type = EventType::TraceOverflow; e.header.flags = 1;
            e.header.sequence = sequence_.fetch_add(1); e.header.timestamp_ns = monotonic_time_ns(); e.header.payload_bytes = sizeof(count);
            std::memcpy(e.payload.data(), &count, sizeof(count)); if (!writer.append({&e, 1})) { failed_ = true; }
        }
    } catch (...) { failed_ = true; }
}
void MultiSession::finish() {
    if (stopped_) { return; }
    stopping_.store(true, std::memory_order_release); collector_.join(); stopped_ = true;
    try {
        auto trace = TraceReader::inspect(path_); if (trace.status != TraceReader::Status::Complete) { failed_ = true; }
        for (const auto& e : trace.events) { graph_.consume(e); } graph_.analyze();
        std::ofstream metadata(path_.string() + ".session.json");
        metadata << "{\"schema\":1,\"producers\":" << rings_.size() << ",\"complete\":" << (complete() ? "true" : "false") << ",\"dropped_events\":" << dropped() << "}\n";
        if (!metadata) { failed_ = true; }
    } catch (...) { failed_ = true; }
}
}
