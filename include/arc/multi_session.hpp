#pragma once
#include "arc/session.hpp"
namespace arc {
// Producer slots are provisioned before capture and never resized. Each slot
// remains SPSC; stable global sequence IDs allow offline causal reconstruction.
class MultiSession {
public:
    MultiSession(const std::filesystem::path& path, std::size_t producers, std::size_t capacity = 16384);
    ~MultiSession();
    MultiSession(const MultiSession&) = delete;
    MultiSession& operator=(const MultiSession&) = delete;
    template<class T> bool emit(std::size_t producer, EventType type, const T& payload) noexcept {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= kMaxEventPayloadBytes);
        if (producer >= rings_.size()) { return false; }
        Event e{}; e.header.type = type; e.header.flags = 1;
        e.header.sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
        e.header.timestamp_ns = monotonic_time_ns(); e.header.thread_id = static_cast<std::uint32_t>(producer + 1);
        e.header.payload_bytes = sizeof(T); std::memcpy(e.payload.data(), &payload, sizeof(T));
        return rings_[producer]->try_emit(e);
    }
    EventRing& ring(std::size_t producer) { return *rings_.at(producer); }
    std::atomic<std::uint64_t>& sequence() noexcept { return sequence_; }
    void finish();
    bool complete() const noexcept { return stopped_ && !failed_ && dropped() == 0 && graph_.errors() == 0; }
    std::uint64_t dropped() const noexcept;
    const ResourceGraph& graph() const noexcept { return graph_; } // after finish
private:
    std::filesystem::path path_;
    std::vector<std::unique_ptr<EventRing>> rings_;
    std::atomic<std::uint64_t> sequence_{1};
    std::atomic<bool> stopping_{false};
    bool stopped_{}, failed_{};
    ResourceGraph graph_;
    std::thread collector_;
    void collect();
};
}
