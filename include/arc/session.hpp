#pragma once
#include "arc/clock.hpp"
#include "arc/event_ring.hpp"
#include "arc/resource_graph.hpp"
#include "arc/trace.hpp"
#include <atomic>
#include <cstring>
#include <thread>

namespace arc {
enum class ObserverMode { Light, Full };

// One producer per Session. Integrations with multiple API threads give each
// producer a separate stream; callers must stop producing before destruction.
class Session {
public:
    Session(const std::filesystem::path& trace, std::size_t capacity = 16384);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    template<class T> bool emit(EventType type, const T& payload) noexcept {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= kMaxEventPayloadBytes);
        Event event{}; event.header.type = type; event.header.sequence = sequence_++;
        event.header.timestamp_ns = monotonic_time_ns(); event.header.thread_id = 1;
        event.header.payload_bytes = sizeof(T);
        std::memcpy(event.payload.data(), &payload, sizeof(T));
        return ring_.try_emit(event);
    }
    void finish();
    EventRing& ring() noexcept { return ring_; }
    const ResourceGraph& graph() const { return graph_; } // only after finish
    bool complete() const noexcept { return stopped_ && !io_error_ && ring_.dropped_events() == 0; }
    std::size_t dropped() const noexcept { return ring_.dropped_events(); }
private:
    std::filesystem::path trace_path_;
    EventRing ring_;
    TraceWriter writer_;
    ResourceGraph graph_;
    std::uint64_t sequence_{1};
    std::atomic<bool> stopping_{false};
    bool stopped_{};
    bool io_error_{};
    std::thread collector_;
    void collect();
};
}
