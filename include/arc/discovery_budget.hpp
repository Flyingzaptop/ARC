#pragma once

#include <algorithm>
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
    };

    DiscoveryBudget() noexcept = default;
    explicit DiscoveryBudget(Config config) noexcept : config_(config) {}
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
            if (this != &other) { release(); owner_ = other.owner_; start_ = other.start_; epoch_ = other.epoch_; events_ = other.events_; bytes_ = other.bytes_; incomplete_ = other.incomplete_; finished_ = other.finished_; other.owner_ = nullptr; }
            return *this;
        }
        ~CaptureLease() { release(); }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
        bool valid() const noexcept { return owner_ && !incomplete_ && owner_->epoch_.load(std::memory_order_acquire) == epoch_; }
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
            owner_->captures_.fetch_sub(1, std::memory_order_acq_rel);
            owner_ = nullptr;
        }
    private:
        friend class DiscoveryBudget;
        CaptureLease(DiscoveryBudget* owner, Time start, std::uint64_t epoch) noexcept : owner_(owner), start_(start), epoch_(epoch) {}
        DiscoveryBudget* owner_{};
        Time start_{};
        std::uint64_t epoch_{}, events_{};
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

    // A new context only marks pending novelty. It cannot bypass cooldown.
    void note_novelty() noexcept { novelty_.store(true, std::memory_order_release); }
    bool ready(Time now = Clock::now()) const noexcept {
        return novelty_.load(std::memory_order_acquire) && ticks(now) >= retry_after_.load(std::memory_order_acquire);
    }
    void discovery_finished(bool success, Time now = Clock::now()) noexcept {
        if (success) { failures_.store(0, std::memory_order_relaxed); novelty_.store(false, std::memory_order_release); return; }
        const unsigned failures = failures_.fetch_add(1, std::memory_order_acq_rel);
        retry_after_.store(ticks(now + config_.retry_backoff[std::min(failures, 3u)]), std::memory_order_release);
        novelty_.store(true, std::memory_order_release);
    }
    std::optional<CaptureLease> try_begin_capture(Time now = Clock::now()) noexcept {
        if (!ready(now) || !config_.cpu_captures) return reject_capture();
        const auto epoch = epoch_.load(std::memory_order_acquire);
        unsigned active = captures_.load(std::memory_order_relaxed);
        while (active < config_.cpu_captures) {
            if (captures_.compare_exchange_weak(active, active + 1, std::memory_order_acq_rel)) {
                auto next = next_capture_.load(std::memory_order_acquire);
                const auto until = ticks(now + config_.capture_spacing);
                if (ticks(now) >= next && next_capture_.compare_exchange_strong(next, until, std::memory_order_acq_rel)) {
                    CaptureLease lease(this, now, epoch);
                    if (!lease.valid()) return reject_capture();
                    return std::optional<CaptureLease>(std::move(lease));
                }
                captures_.fetch_sub(1, std::memory_order_acq_rel);
                break;
            }
        }
        return reject_capture();
    }
    void cancel() noexcept { epoch_.fetch_add(1, std::memory_order_acq_rel); novelty_.store(false, std::memory_order_release); }
    Snapshot snapshot() const noexcept {
        return {jobs_.load(), captures_.load(), jobs_high_.load(), bytes_.load(), bytes_high_.load(),
                epoch_.load(), rejected_jobs_.load(), rejected_captures_.load(), incomplete_captures_.load()};
    }
    const Config& config() const noexcept { return config_; }

private:
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
    std::atomic<bool> novelty_{true};
};

} // namespace arc
