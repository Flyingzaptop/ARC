#pragma once

#include "arc/discovery_budget.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace arc::cpu::dbi {

// One selected basic-block study at a time. Calls on a translated application
// block must first pass the cheap main-module address check. This object does
// not establish instruction semantics or admit a transformation.
class DiscoveryCapture {
public:
    enum class Observation : std::uint8_t {
        Started, Recorded, OtherRegion, Waiting, Contended, Incomplete, Cancelled
    };
    struct Snapshot {
        std::uintptr_t selected_region{};
        std::uint64_t events{}, live_in_mask{}, code_hash{};
        std::size_t bytes{};
        std::uint64_t started{}, completed{}, incomplete{}, contended{};
        bool active{}, cancelled{}, last_complete{};
        arc::DiscoveryBudget::Snapshot budget{};
    };

    DiscoveryCapture() noexcept = default;
    DiscoveryCapture(const DiscoveryCapture&) = delete;
    DiscoveryCapture& operator=(const DiscoveryCapture&) = delete;
    const arc::DiscoveryBudget::Config& config() const noexcept { return budget_.config(); }

    // Re-arm only on actual novelty. Repeated translations do not defeat the
    // one-second spacing or failure backoff in DiscoveryBudget.
    void note_novelty() noexcept { budget_.note_novelty(); }
    bool note_novelty(std::uint64_t key) noexcept { return budget_.note_novelty(key); }
    bool forget_novelty(std::uint64_t key) noexcept { return budget_.forget_novelty(key); }

    // Nonblocking on application threads. A selected-region contention marks
    // its trace incomplete, because that event might contain a required fact.
    Observation try_observe(std::uintptr_t region_key, std::uint64_t events,
                            std::size_t bytes, std::uint32_t live_in_mask = 0,
                            std::uint64_t code_hash = 0,
                            arc::DiscoveryBudget::Time now = arc::DiscoveryBudget::Clock::now()) noexcept {
        if (cancelled_.load(std::memory_order_acquire)) return Observation::Cancelled;
        if (gate_.test_and_set(std::memory_order_acquire)) {
            contended_.fetch_add(1, std::memory_order_relaxed);
            if (active_.load(std::memory_order_acquire) && region_key &&
                region_key == selected_.load(std::memory_order_acquire))
                loss_pending_.store(true, std::memory_order_release);
            return Observation::Contended;
        }
        FlagGuard lock(gate_);
        if (cancelled_.load(std::memory_order_acquire)) {
            close(false, now);
            return Observation::Cancelled;
        }
        if (loss_pending_.exchange(false, std::memory_order_acq_rel)) {
            close(false, now);
            return Observation::Incomplete;
        }
        bool started = false;
        if (!capture_) {
            auto lease = budget_.try_begin_capture(now, region_key);
            if (!lease) return Observation::Waiting;
            capture_ = std::move(*lease);
            capture_epoch_ = capture_->epoch();
            capture_ticket_ = capture_->ticket();
            selected_.store(region_key, std::memory_order_release);
            active_.store(true, std::memory_order_release);
            events_.store(0, std::memory_order_relaxed);
            bytes_.store(0, std::memory_order_relaxed);
            live_in_.store(0, std::memory_order_relaxed);
            code_hash_.store(0, std::memory_order_relaxed);
            started_.fetch_add(1, std::memory_order_relaxed);
            started = true;
        }
        if (!capture_->record(0, 0, now)) {
            close(false, now);
            return Observation::Incomplete;
        }
        if (region_key != selected_.load(std::memory_order_relaxed)) return Observation::OtherRegion;
        const auto previous_hash = code_hash_.load(std::memory_order_relaxed);
        if (previous_hash && code_hash && previous_hash != code_hash) {
            close(false, now);
            return Observation::Incomplete;
        }
        if (!capture_->record(events, bytes, now)) {
            close(false, now);
            return Observation::Incomplete;
        }
        events_.fetch_add(events, std::memory_order_relaxed);
        bytes_.fetch_add(bytes, std::memory_order_relaxed);
        live_in_.fetch_or(live_in_mask, std::memory_order_relaxed);
        if (code_hash) code_hash_.store(code_hash, std::memory_order_relaxed);
        return started ? Observation::Started : Observation::Recorded;
    }

    // Call at a known region boundary. false also covers a crossing/unknown
    // boundary. A complete static decode is not automatically a complete
    // dynamic memory/dependency trace; caller supplies that distinction.
    bool finish(bool complete,
                arc::DiscoveryBudget::Time now = arc::DiscoveryBudget::Clock::now()) noexcept {
        if (gate_.test_and_set(std::memory_order_acquire)) {
            loss_pending_.store(true, std::memory_order_release);
            return false;
        }
        FlagGuard lock(gate_);
        return close(complete && !loss_pending_.exchange(false, std::memory_order_acq_rel), now);
    }

    void cancel() noexcept {
        cancelled_.store(true, std::memory_order_release);
        budget_.cancel();
        if (!gate_.test_and_set(std::memory_order_acquire)) {
            FlagGuard lock(gate_);
            close(false, arc::DiscoveryBudget::Clock::now());
        } else {
            loss_pending_.store(true, std::memory_order_release);
        }
    }

    Snapshot snapshot() const noexcept {
        return {selected_.load(std::memory_order_acquire),
                events_.load(std::memory_order_relaxed), live_in_.load(std::memory_order_relaxed),
                code_hash_.load(std::memory_order_relaxed), bytes_.load(std::memory_order_relaxed),
                started_.load(std::memory_order_relaxed), completed_.load(std::memory_order_relaxed),
                incomplete_.load(std::memory_order_relaxed), contended_.load(std::memory_order_relaxed),
                active_.load(std::memory_order_acquire), cancelled_.load(std::memory_order_acquire),
                last_complete_.load(std::memory_order_relaxed), budget_.snapshot()};
    }

private:
    static arc::DiscoveryBudget::Config cpu_config() noexcept {
        arc::DiscoveryBudget::Config config;
        config.initial_probe = false;
        return config;
    }
    struct FlagGuard {
        explicit FlagGuard(std::atomic_flag& flag) noexcept : flag_(flag) {}
        ~FlagGuard() noexcept { flag_.clear(std::memory_order_release); }
        FlagGuard(const FlagGuard&) = delete;
        FlagGuard& operator=(const FlagGuard&) = delete;
        std::atomic_flag& flag_;
    };
    bool close(bool complete, arc::DiscoveryBudget::Time now) noexcept {
        if (!capture_) return false;
        if (!complete) capture_->mark_incomplete();
        const bool good = capture_->finish(now) && complete;
        capture_.reset();
        active_.store(false, std::memory_order_release);
        last_complete_.store(good, std::memory_order_release);
        if (good) completed_.fetch_add(1, std::memory_order_relaxed);
        else incomplete_.fetch_add(1, std::memory_order_relaxed);
        budget_.discovery_finished(good, now, capture_epoch_, capture_ticket_);
        return good;
    }

    arc::DiscoveryBudget budget_{cpu_config()};
    std::atomic_flag gate_ = ATOMIC_FLAG_INIT;
    std::optional<arc::DiscoveryBudget::CaptureLease> capture_;
    std::uint64_t capture_epoch_{};
    unsigned capture_ticket_{};
    std::atomic<std::uintptr_t> selected_{};
    std::atomic<std::uint64_t> events_{}, live_in_{}, code_hash_{};
    std::atomic<std::size_t> bytes_{};
    std::atomic<std::uint64_t> started_{}, completed_{}, incomplete_{}, contended_{};
    std::atomic<bool> active_{}, cancelled_{}, last_complete_{}, loss_pending_{};
};

} // namespace arc::cpu::dbi
