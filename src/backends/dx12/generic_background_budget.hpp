#pragma once
#include <mutex>
#include <atomic>
namespace arc::dx12 {
// Only background compiler/critic callers acquire this. Never on render threads.
inline std::timed_mutex& background_compute_gate(){static std::timed_mutex gate;return gate;}
inline std::atomic<unsigned>& background_quality_waiters(){static std::atomic<unsigned> value{};return value;}
}
