#include "arc/event_ring.hpp"

#include <atomic>
#include <stdexcept>
#include <vector>

namespace arc {

struct EventRing::Storage final {
    explicit Storage(const std::size_t requested_capacity)
        : events(requested_capacity + 1), capacity(requested_capacity + 1) {}

    std::vector<Event> events;
    const std::size_t capacity;
    std::atomic<std::size_t> write{0};
    std::atomic<std::size_t> read{0};
    std::atomic<std::size_t> dropped{0};
};

EventRing::EventRing(const std::size_t capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("EventRing capacity must be non-zero");
    }
    storage_ = std::make_unique<Storage>(capacity);
}

EventRing::~EventRing() = default;

bool EventRing::try_emit(const Event& event) noexcept {
    const auto write = storage_->write.load(std::memory_order_relaxed);
    const auto next = (write + 1) % storage_->capacity;
    if (next == storage_->read.load(std::memory_order_acquire)) {
        storage_->dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    storage_->events[write] = event;
    storage_->write.store(next, std::memory_order_release);
    return true;
}

bool EventRing::try_pop(Event& event) noexcept {
    const auto read = storage_->read.load(std::memory_order_relaxed);
    if (read == storage_->write.load(std::memory_order_acquire)) {
        return false;
    }
    event = storage_->events[read];
    storage_->read.store((read + 1) % storage_->capacity, std::memory_order_release);
    return true;
}

std::size_t EventRing::capacity() const noexcept { return storage_->capacity - 1; }
std::size_t EventRing::dropped_events() const noexcept {
    return storage_->dropped.load(std::memory_order_relaxed);
}

}  // namespace arc
