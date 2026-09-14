#pragma once

#include "arc/events.hpp"

#include <cstddef>
#include <memory>

namespace arc {

// Single producer / single consumer ring. One event producer owns each ring;
// the collector is its only consumer. It never blocks or allocates in try_emit.
class EventRing final {
public:
    explicit EventRing(std::size_t capacity);
    ~EventRing();
    EventRing(const EventRing&) = delete;
    EventRing& operator=(const EventRing&) = delete;

    [[nodiscard]] bool try_emit(const Event& event) noexcept;
    [[nodiscard]] bool try_pop(Event& event) noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t dropped_events() const noexcept;

private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
};

}  // namespace arc
