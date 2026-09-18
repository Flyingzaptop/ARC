#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ostream>

namespace arc_wicked {
// Presence of each access direction per command list; deliberately not an event
// sequence. Stage 16 attribution must preserve ordering separately.
struct AccessMask {
    bool read = false;
    bool write = false;
    void observe(bool is_write) noexcept {
        if (is_write) write = true;
        else read = true;
    }
};

// Constant storage, no allocation or mutex in recording. Log2 nanosecond
// histogram bounds are exported, not presented as exact percentiles. Concurrent
// snapshots are approximate; counters are never reset while workers are active.
struct HookTiming {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> total_ns{0};
    std::array<std::atomic<std::uint64_t>, 64> buckets{};
    static unsigned bucket(std::uint64_t ns) noexcept {
        unsigned index = 0;
        while (ns > 1 && index < 63) { ns >>= 1; ++index; }
        return index;
    }
    void record(std::uint64_t ns) noexcept {
        total_ns.fetch_add(ns, std::memory_order_relaxed);
        buckets[bucket(ns)].fetch_add(1, std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
    }
    void write_json(std::ostream& out) const {
        out << "{\"calls\":" << calls.load(std::memory_order_relaxed)
            << ",\"total_ns\":" << total_ns.load(std::memory_order_relaxed)
            << ",\"log2_ns_buckets\":[";
        for (unsigned i = 0; i < buckets.size(); ++i) {
            if (i) out << ',';
            out << buckets[i].load(std::memory_order_relaxed);
        }
        out << "]}";
    }
};

class HookTimer {
public:
    explicit HookTimer(HookTiming& timing) noexcept
        : timing_(timing), start_(std::chrono::steady_clock::now()) {}
    ~HookTimer() {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_).count();
        timing_.record(ns > 0 ? static_cast<std::uint64_t>(ns) : 0);
    }
    HookTimer(const HookTimer&) = delete;
    HookTimer& operator=(const HookTimer&) = delete;
private:
    HookTiming& timing_;
    std::chrono::steady_clock::time_point start_;
};
} // namespace arc_wicked
