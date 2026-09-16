#include "arc/clock.hpp"

#include <chrono>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace arc {

std::uint64_t monotonic_time_ns() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::uint64_t current_thread_cpu_time_ns() noexcept {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) { return 0; }
    const auto ticks = [](FILETIME value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100;
#else
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) { return 0; }
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(value.tv_nsec);
#endif
}

}  // namespace arc
