#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace arc {

// Admission is nonblocking and allocation-free. Jobs may finish after cancel(),
// but their epoch becomes invalid and their results must not be published.
class DiscoveryBudget {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    struct Config {
        unsigned heavy_workers = 1;
        unsigned outstanding_jobs = 8;
        std::size_t outstanding_bytes = 64u * 1024u * 1024u;
        unsigned cpu_captures = 1;
        bool initial_probe = true;
        std::chrono::milliseconds gpu_capture_window{10000};
        std::uint64_t gpu_capture_events = 100000;
        std::size_t gpu_capture_bytes = 8u * 1024u * 1024u;
        std::chrono::milliseconds capture_window{100};
        std::uint64_t capture_events = 100'000;
        std::size_t capture_bytes = 8u * 1024u * 1024u;
        std::chrono::milliseconds capture_spacing{1000};
        std::chrono::seconds retry_backoff[4]{std::chrono::seconds{4},std::chrono::seconds{8},std::chrono::seconds{16},std::chrono::seconds{30}};
    };
    struct Snapshot {
        unsigned active_jobs{}, active_captures{}, jobs_high_water{};
        std::size_t bytes_in_flight{}, bytes_high_water{};
        std::uint64_t epoch{}, rejected_jobs{}, rejected_captures{}, incomplete_captures{};
        std::uint64_t pending_novelty{}, novelty_overflow{};
    };

    DiscoveryBudget() noexcept { init_novelty(); }
    explicit DiscoveryBudget(Config config) noexcept : config_(config) { init_novelty(); }
    DiscoveryBudget(const DiscoveryBudget&) = delete;
    DiscoveryBudget& operator=(const DiscoveryBudget&) = delete;

    class JobLease {
    public:
        JobLease() = default;
        JobLease(const JobLease&) = delete;
        JobLease& operator=(const JobLease&) = delete;
        JobLease(JobLease&& other) noexcept { *this = std::move(other); }
        JobLease& operator=(JobLease&& other) noexcept {
            if (this != &other) { release(); owner_ = other.owner_; bytes_ = other.bytes_; epoch_ = other.epoch_; other.owner_ = nullptr; }
            return *this;
        }
        ~JobLease() { release(); }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
        bool valid() const noexcept { return owner_ && owner_->epoch_.load(std::memory_order_acquire) == epoch_; }
        std::uint64_t epoch() const noexcept { return epoch_; }
        void release() noexcept {
            if (!owner_) return;
            owner_->bytes_.fetch_sub(bytes_, std::memory_order_acq_rel);
            owner_->jobs_.fetch_sub(1, std::memory_order_acq_rel);
            owner_ = nullptr;
        }
    private:
        friend class DiscoveryBudget;
        JobLease(DiscoveryBudget* owner, std::size_t bytes, std::uint64_t epoch) noexcept : owner_(owner), bytes_(bytes), epoch_(epoch) {}
        DiscoveryBudget* owner_{};
        std::size_t bytes_{};
        std::uint64_t epoch_{};
    };

    class CaptureLease {
    public:
        CaptureLease() = default;
        CaptureLease(const CaptureLease&) = delete;
        CaptureLease& operator=(const CaptureLease&) = delete;
        CaptureLease(CaptureLease&& other) noexcept { *this = std::move(other); }
        CaptureLease& operator=(CaptureLease&& other) noexcept {
            if (this != &other) { release(); owner_ = other.owner_; start_ = other.start_; epoch_ = other.epoch_; ticket_ = other.ticket_; events_ = other.events_; bytes_ = other.bytes_; incomplete_ = other.incomplete_; finished_ = other.finished_; other.owner_ = nullptr; }
            return *this;
        }
        ~CaptureLease() { release(); }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
        bool valid() const noexcept { return owner_ && !incomplete_ && owner_->epoch_.load(std::memory_order_acquire) == epoch_; }
        std::uint64_t epoch() const noexcept { return epoch_; }
        unsigned ticket() const noexcept { return ticket_; }
        // false means the trace crossed a bound and cannot support a complete contract.
        bool record(std::uint64_t events, std::size_t bytes, Time now = Clock::now()) noexcept {
            if (!valid() || now - start_ >= owner_->config_.capture_window ||
                events > owner_->config_.capture_events - events_ ||
                bytes > owner_->config_.capture_bytes - bytes_) { mark_incomplete(); return false; }
            events_ += events; bytes_ += bytes;
            if (events_ == owner_->config_.capture_events || bytes_ == owner_->config_.capture_bytes) { mark_incomplete(); return false; }
            return true;
        }
        std::uint64_t events() const noexcept { return events_; }
        std::size_t bytes() const noexcept { return bytes_; }
        bool incomplete() const noexcept { return incomplete_ || (owner_ && !valid()); }
        bool finish(Time now = Clock::now()) noexcept {
            if (!owner_) return false;
            if (now - start_ >= owner_->config_.capture_window || !valid()) mark_incomplete();
            finished_ = true;
            release();
            return !incomplete_;
        }
        void mark_incomplete() noexcept {
            if (owner_ && !incomplete_) { incomplete_ = true; owner_->incomplete_captures_.fetch_add(1, std::memory_order_relaxed); }
        }
        void release() noexcept {
            if (!owner_) return;
            if (!finished_ || owner_->epoch_.load(std::memory_order_acquire) != epoch_) mark_incomplete();
            if (incomplete_) owner_->restore_capture_ticket(ticket_, epoch_);
            owner_->captures_.fetch_sub(1, std::memory_order_acq_rel);
            owner_ = nullptr;
        }
    private:
        friend class DiscoveryBudget;
        CaptureLease(DiscoveryBudget* owner, Time start, std::uint64_t epoch, unsigned ticket) noexcept : owner_(owner), start_(start), epoch_(epoch), ticket_(ticket) {}
        DiscoveryBudget* owner_{};
        Time start_{};
        std::uint64_t epoch_{}, events_{};
        unsigned ticket_{novelty_slots};
        std::size_t bytes_{};
        bool incomplete_{};
        bool finished_{};
    };

    // Caller owns the bounded queue. Reserve before enqueue and retain the
    // lease until completion; no reservation is kept after a failed enqueue.
    std::optional<JobLease> try_reserve(std::size_t bytes) noexcept {
        if (!bytes || bytes > config_.outstanding_bytes || !config_.outstanding_jobs) return reject_job();
        const auto epoch = epoch_.load(std::memory_order_acquire);
        unsigned jobs = jobs_.load(std::memory_order_relaxed);
        while (jobs < config_.outstanding_jobs) {
            if (jobs_.compare_exchange_weak(jobs, jobs + 1, std::memory_order_acq_rel)) {
                std::size_t current = bytes_.load(std::memory_order_relaxed);
                while (current <= config_.outstanding_bytes - bytes) {
                    if (bytes_.compare_exchange_weak(current, current + bytes, std::memory_order_acq_rel)) {
                        high_water(jobs_high_, jobs + 1);
                        high_water(bytes_high_, current + bytes);
                        JobLease lease(this, bytes, epoch);
                        if (!lease.valid()) return reject_job();
                        return std::optional<JobLease>(std::move(lease));
                    }
                }
                jobs_.fetch_sub(1, std::memory_order_acq_rel);
                break;
            }
        }
        return reject_job();
    }

    // Stable nonzero generation (< 2^62-1); UINT64_MAX is the legacy token.
    // Store the identity exactly, never a truncated hash that aliases work. A key
    // has one pending/active ticket. Completion frees the slot, so a later
    // observation can re-arm even after many generations. No allocation/lock.
    bool note_novelty(std::uint64_t key) noexcept {
        NoveltyWriter writer(novelty_writers_);
        if (!key || cancelled_.load(std::memory_order_acquire)) return false;
        const auto epoch = epoch_.load(std::memory_order_acquire);
        const auto identity = novelty_identity(key);
        if (!identity) { novelty_overflow_.fetch_add(1, std::memory_order_relaxed); return false; }
        const auto pending_word = (identity << 2) | 1u;
        const auto start = identity % novelty_slots;
        // Scan the full table before insertion: completed slots create gaps,
        // so stopping at the first gap could duplicate a key farther along.
        for (std::size_t attempt = 0; attempt < novelty_slots; ++attempt) {
            std::size_t empty = novelty_slots;
            for (std::size_t probe = 0; probe < novelty_slots; ++probe) {
                const auto index = (start + probe) % novelty_slots;
                const auto value = novelty_keys_[index].load(std::memory_order_acquire);
                if (value != 0 && (value >> 2) == identity) return false;
                if (value == 0 && empty == novelty_slots) empty = index;
            }
            if (empty == novelty_slots) break;
            auto expected = std::uint64_t{0};
            auto& slot = novelty_keys_[empty];
            if (slot.compare_exchange_strong(expected, pending_word, std::memory_order_acq_rel)) {
                pending_novelty_.fetch_add(1, std::memory_order_release);
                if (cancelled_.load(std::memory_order_acquire) || epoch != epoch_.load(std::memory_order_acquire)) {
                    expected = pending_word;
                    slot.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
                    pending_novelty_.store(0, std::memory_order_release);
                    return false;
                }
                return true;
            }
        }
        novelty_overflow_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Compatibility token for callers without a stable identity. Repeated
    // calls coalesce while pending; success frees the token for later work.
    void note_novelty() noexcept { (void)note_novelty(legacy_novelty_key); }
    // Eviction removes only a pending ticket. Active work must finish/cancel
    // through its lease and cannot be mistaken for another region's ticket.
    bool forget_novelty(std::uint64_t key) noexcept {
        if (!key) return false;
        const auto identity = novelty_identity(key);
        if (!identity) return false;
        for (auto& slot : novelty_keys_) {
            auto word = slot.load(std::memory_order_acquire);
            if ((word >> 2) != identity || (word & 3u) != 1u) continue;
            if (!slot.compare_exchange_strong(word, 0, std::memory_order_acq_rel)) return false;
            auto pending = pending_novelty_.load(std::memory_order_acquire);
            while (pending && !pending_novelty_.compare_exchange_weak(
                pending, pending - 1, std::memory_order_acq_rel)) {}
            return true;
        }
        return false;
    }
    bool ready(Time now = Clock::now()) const noexcept {
        return !cancelled_.load(std::memory_order_acquire) && pending_novelty_.load(std::memory_order_acquire) != 0 &&
               ticks(now) >= retry_after_.load(std::memory_order_acquire);
    }
    void discovery_finished(bool success, Time now = Clock::now(), std::uint64_t work_epoch = 0,
                            unsigned ticket = novelty_slots) noexcept {
        if (work_epoch && work_epoch != epoch_.load(std::memory_order_acquire)) return;
        if (cancelled_.load(std::memory_order_acquire)) return;
        if (ticket == novelty_slots) ticket = active_ticket_.load(std::memory_order_acquire);
        if (ticket >= novelty_slots) return;
        auto& slot = novelty_keys_[ticket];
        auto active_word = slot.load(std::memory_order_acquire);
        if ((active_word & 3u) != 2u && (success || (active_word & 3u) != 1u)) return;
        if (success) {
            if (!slot.compare_exchange_strong(active_word, 0, std::memory_order_acq_rel)) return;
            failures_.store(0, std::memory_order_relaxed);
            return;
        }
        if ((active_word & 3u) == 2u) restore_capture_ticket(ticket, work_epoch);
        const unsigned failures = failures_.fetch_add(1, std::memory_order_acq_rel);
        retry_after_.store(ticks(now + config_.retry_backoff[std::min(failures, 3u)]), std::memory_order_release);
    }
    std::optional<CaptureLease> try_begin_capture(Time now = Clock::now(), std::uint64_t requested_key = 0) noexcept {
        if (!ready(now) || !config_.cpu_captures) return reject_capture();
        const auto requested_identity = requested_key ? novelty_identity(requested_key) : 0;
        if (requested_key && !requested_identity) return reject_capture();
        if (requested_identity && !has_pending_ticket(requested_identity)) return reject_capture();
        const auto epoch = epoch_.load(std::memory_order_acquire);
        unsigned active = captures_.load(std::memory_order_relaxed);
        while (active < std::min(config_.cpu_captures, 1u)) {
            if (captures_.compare_exchange_weak(active, active + 1, std::memory_order_acq_rel)) {
                auto next = next_capture_.load(std::memory_order_acquire);
                const auto until = ticks(now + config_.capture_spacing);
                if (ticks(now) >= next && next_capture_.compare_exchange_strong(next, until, std::memory_order_acq_rel)) {
                    for (unsigned i = 0; i < novelty_slots; ++i) {
                        auto pending_word = novelty_keys_[i].load(std::memory_order_acquire);
                        if ((pending_word & 3u) != 1u) continue;
                        if (requested_identity && (pending_word >> 2) != requested_identity) continue;
                        if (novelty_keys_[i].compare_exchange_strong(pending_word, (pending_word & ~3ull) | 2u,
                                                                      std::memory_order_acq_rel)) {
                            auto pending = pending_novelty_.load(std::memory_order_acquire);
                            while (pending && !pending_novelty_.compare_exchange_weak(
                                pending, pending - 1, std::memory_order_acq_rel)) {}
                            active_ticket_.store(i, std::memory_order_release);
                            CaptureLease lease(this, now, epoch, i);
                            if (!lease.valid()) return reject_capture();
                            return std::optional<CaptureLease>(std::move(lease));
                        }
                    }
                }
                captures_.fetch_sub(1, std::memory_order_acq_rel);
                break;
            }
        }
        return reject_capture();
    }
    void cancel() noexcept {
        cancelled_.store(true, std::memory_order_release);
        epoch_.fetch_add(1, std::memory_order_acq_rel);
        pending_novelty_.store(0, std::memory_order_release);
        for (auto& slot : novelty_keys_) slot.store(0, std::memory_order_release);
        active_ticket_.store(novelty_slots, std::memory_order_release);
    }
    // A later session may explicitly re-arm after old capture leases drain.
    bool resume() noexcept {
        if (captures_.load(std::memory_order_acquire) != 0 ||
            novelty_writers_.load(std::memory_order_acquire) != 0) return false;
        retry_after_.store(0, std::memory_order_release);
        failures_.store(0, std::memory_order_release);
        cancelled_.store(false, std::memory_order_release);
        return true;
    }
    Snapshot snapshot() const noexcept {
        return {jobs_.load(), captures_.load(), jobs_high_.load(), bytes_.load(), bytes_high_.load(),
                epoch_.load(), rejected_jobs_.load(), rejected_captures_.load(), incomplete_captures_.load(),
                pending_novelty_.load(), novelty_overflow_.load()};
    }
    const Config& config() const noexcept { return config_; }

private:
    bool has_pending_ticket(std::uint64_t identity) const noexcept {
        for (const auto& slot : novelty_keys_) {
            const auto word = slot.load(std::memory_order_acquire);
            if ((word & 3u) == 1u && (word >> 2) == identity) return true;
        }
        return false;
    }
    struct NoveltyWriter {
        explicit NoveltyWriter(std::atomic<unsigned>& writers) noexcept : writers_(writers) {
            writers_.fetch_add(1, std::memory_order_acq_rel);
        }
        ~NoveltyWriter() noexcept { writers_.fetch_sub(1, std::memory_order_acq_rel); }
        std::atomic<unsigned>& writers_;
    };
    void restore_capture_ticket(unsigned ticket, std::uint64_t work_epoch) noexcept {
        if (ticket >= novelty_slots || work_epoch != epoch_.load(std::memory_order_acquire) ||
            cancelled_.load(std::memory_order_acquire)) return;
        auto& slot = novelty_keys_[ticket];
        auto active_word = slot.load(std::memory_order_acquire);
        if ((active_word & 3u) == 2u &&
            slot.compare_exchange_strong(active_word, (active_word & ~3ull) | 1u, std::memory_order_acq_rel)) {
            pending_novelty_.fetch_add(1, std::memory_order_release);
            if (cancelled_.load(std::memory_order_acquire) || work_epoch != epoch_.load(std::memory_order_acquire))
                pending_novelty_.store(0, std::memory_order_release);
        }
    }
    static constexpr std::size_t novelty_slots = 256;
    static constexpr std::uint64_t legacy_novelty_key = UINT64_MAX;
    static std::uint64_t novelty_identity(std::uint64_t key) noexcept {
        constexpr auto legacy = (1ull << 62) - 1;
        return key == legacy_novelty_key ? legacy : (key < legacy ? key : 0);
    }
    void init_novelty() noexcept {
        if (config_.initial_probe) {
            novelty_keys_[0].store((novelty_identity(0x9e3779b97f4a7c15ull) << 2) | 1u,
                                   std::memory_order_relaxed);
            pending_novelty_.store(1, std::memory_order_relaxed);
        }
    }
    static std::int64_t ticks(Time time) noexcept { return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(); }
    template<class T> static void high_water(std::atomic<T>& peak, T value) noexcept {
        T current = peak.load(std::memory_order_relaxed);
        while (current < value && !peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
    }
    std::optional<JobLease> reject_job() noexcept { rejected_jobs_.fetch_add(1, std::memory_order_relaxed); return std::nullopt; }
    std::optional<CaptureLease> reject_capture() noexcept { rejected_captures_.fetch_add(1, std::memory_order_relaxed); return std::nullopt; }
    Config config_;
    std::atomic<unsigned> jobs_{}, captures_{}, jobs_high_{}, failures_{};
    std::atomic<std::size_t> bytes_{}, bytes_high_{};
    std::atomic<std::uint64_t> epoch_{1}, rejected_jobs_{}, rejected_captures_{}, incomplete_captures_{};
    std::atomic<std::int64_t> next_capture_{}, retry_after_{};
    std::atomic<std::uint64_t> pending_novelty_{}, novelty_overflow_{};
    std::atomic<unsigned> active_ticket_{novelty_slots};
    std::atomic<unsigned> novelty_writers_{};
    std::atomic<bool> cancelled_{};
    std::array<std::atomic<std::uint64_t>, novelty_slots> novelty_keys_{};
};

} // namespace arc
