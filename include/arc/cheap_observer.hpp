#pragma once

#include "arc/event_ring.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#endif

namespace arc {

// Creation hints are only discovery signals. They do not establish a safe
// transformation contract. Diagnostic loss and correctness loss stay separate.
class CheapObserver final {
public:
    static constexpr unsigned producer_slots = 64;
    static constexpr unsigned events_per_slot = 64;
    struct CreationHint { std::uint64_t opaque_identity{}, size{}, kind{}, present_count{}; };
    struct Snapshot {
        std::uint64_t novelty{}, creations{}, presents{}, submissions{}, present_duration_us{};
        std::uint64_t diagnostic_drops{}, correctness_epoch{};
        unsigned claimed_slots_high_water{};
    };

    CheapObserver() : instance_(next_instance_.fetch_add(1, std::memory_order_relaxed)) {}
    CheapObserver(const CheapObserver&) = delete;
    CheapObserver& operator=(const CheapObserver&) = delete;

    void notify_creation(std::uint16_t kind, std::uint64_t opaque_identity, std::uint64_t size) noexcept {
        creations_.fetch_add(1, std::memory_order_relaxed);
        novelty_.fetch_add(1, std::memory_order_release);
        const auto slot = thread_slot();
        if (slot >= producer_slots) { diagnostic_drops_.fetch_add(1, std::memory_order_relaxed); return; }
        Event event{};
        event.header.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        event.header.sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef _WIN32
        event.header.thread_id = GetCurrentThreadId();
#else
        event.header.thread_id = slot + 1;
#endif
        event.header.type = EventType::TelemetrySample;
        event.header.flags = 1; // CheapObserver creation hint
        event.header.payload_bytes = sizeof(CreationHint);
        const CreationHint hint{opaque_identity, size, kind, presents_.load(std::memory_order_relaxed)};
        static_assert(std::is_trivially_copyable_v<CreationHint> && sizeof(CreationHint) <= kMaxEventPayloadBytes);
        std::memcpy(event.payload.data(), &hint, sizeof(hint));
        if (!slots_[slot].ring.try_emit(event)) diagnostic_drops_.fetch_add(1, std::memory_order_relaxed);
    }

    void observe_present(double duration_ms) noexcept {
        presents_.fetch_add(1, std::memory_order_relaxed);
        if (std::isfinite(duration_ms) && duration_ms >= 0 && duration_ms <= 60'000)
            present_duration_us_.fetch_add(static_cast<std::uint64_t>(duration_ms * 1000.0), std::memory_order_relaxed);
    }
    void observe_submission() noexcept { submissions_.fetch_add(1, std::memory_order_relaxed); }

    // Called by an owner whose correctness-relevant event/assumption was lost.
    // Executors compare the epoch captured at admission with this value.
    void mark_correctness_loss() noexcept { correctness_epoch_.fetch_add(1, std::memory_order_acq_rel); }
    std::uint64_t correctness_epoch() const noexcept { return correctness_epoch_.load(std::memory_order_acquire); }
    std::uint64_t novelty() const noexcept { return novelty_.load(std::memory_order_acquire); }

    Snapshot snapshot() const noexcept {
        return {novelty(), creations_.load(), presents_.load(), submissions_.load(), present_duration_us_.load(),
                diagnostic_drops_.load(), correctness_epoch(), claimed_.load()};
    }
    // One collector calls drain; it may run concurrently with producers.
    template<class Consumer> std::uint64_t drain(Consumer&& consume, std::uint64_t maximum = 512) {
        std::uint64_t count{};
        const auto claimed = claimed_.load(std::memory_order_acquire);
        if (!claimed || !maximum) return 0;
        const auto start = collector_cursor_ % claimed;
        for (unsigned offset = 0; offset < claimed; ++offset) {
            const auto slot = (start + offset) % claimed;
            Event event;
            while (count < maximum && slots_[slot].ring.try_pop(event)) { consume(event); ++count; }
            if (count == maximum) { collector_cursor_ = (slot + 1) % claimed; return count; }
        }
        collector_cursor_ = (start + 1) % claimed;
        return count;
    }

private:
    struct Slot { EventRing ring{events_per_slot}; };
    struct ThreadCache { const CheapObserver* owner{}; std::uint64_t instance{}; unsigned slot{}; };
    unsigned thread_slot() noexcept {
        // A thread may visit more than one observer. Instance identity also
        // prevents a new observer at a recycled address using a stale slot.
        static thread_local std::array<ThreadCache, 4> cache{};
        static thread_local unsigned replacement{};
        for (const auto& entry : cache) if (entry.owner == this && entry.instance == instance_) return entry.slot;
        unsigned claimed = claimed_.load(std::memory_order_relaxed);
        while (claimed < producer_slots) {
            if (claimed_.compare_exchange_weak(claimed, claimed + 1, std::memory_order_acq_rel)) {
                cache[replacement++ % cache.size()] = {this, instance_, claimed};
                return claimed;
            }
        }
        return producer_slots;
    }
    inline static std::atomic<std::uint64_t> next_instance_{1};
    const std::uint64_t instance_;
    std::array<Slot, producer_slots> slots_{};
    unsigned collector_cursor_{}; // single collector only
    std::atomic<unsigned> claimed_{};
    std::atomic<std::uint64_t> sequence_{};
    std::atomic<std::uint64_t> novelty_{}, creations_{}, presents_{}, submissions_{}, present_duration_us_{};
    std::atomic<std::uint64_t> diagnostic_drops_{}, correctness_epoch_{1};
};

} // namespace arc
