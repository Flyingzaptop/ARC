#include "arc/multi_session.hpp"
#include <array>
#include <fstream>
#include <optional>
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
        TraceWriter writer(path_);
        std::array<Event, 1024> batch{};
        std::vector<std::optional<Event>> heads(rings_.size());
        std::uint64_t expectedSequence = 1;
        for (;;) {
            for (std::size_t producer = 0; producer < rings_.size(); ++producer) {
                if (!heads[producer]) {
                    Event event{};
                    if (rings_[producer]->try_pop(event)) { heads[producer] = event; }
                }
            }
            std::size_t count{};
            while (count < batch.size()) {
                std::size_t selected = heads.size();
                for (std::size_t producer = 0; producer < heads.size(); ++producer) {
                    if (heads[producer] && heads[producer]->header.sequence == expectedSequence) {
                        selected = producer; break;
                    }
                }
                if (selected == heads.size()) { break; }
                batch[count++] = *heads[selected]; heads[selected].reset(); ++expectedSequence;
                Event next{};
                if (rings_[selected]->try_pop(next)) { heads[selected] = next; }
            }
            if (count) {
                if (!writer.append({batch.data(), count})) { failed_ = true; }
                continue;
            }
            const bool noHeads = std::none_of(heads.begin(), heads.end(), [](const auto& head) { return head.has_value(); });
            if (stopping_.load(std::memory_order_acquire)) {
                if (!noHeads || dropped()) {
                    failed_ = true;
                    const auto countDropped = dropped();
                    Event overflow{}; overflow.header.type = EventType::TraceOverflow; overflow.header.flags = 1;
                    overflow.header.sequence = expectedSequence; overflow.header.timestamp_ns = monotonic_time_ns(); overflow.header.payload_bytes = sizeof(countDropped);
                    std::memcpy(overflow.payload.data(), &countDropped, sizeof(countDropped));
                    if (!writer.append({&overflow, 1})) { failed_ = true; }
                }
                break;
            }
            std::this_thread::yield();
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
