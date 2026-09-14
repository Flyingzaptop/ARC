#pragma once

#include <cstdint>

namespace arc {

// Returns a monotonic timestamp measured from an unspecified process-local
// epoch. It is suitable for event ordering, never for wall-clock display.
[[nodiscard]] std::uint64_t monotonic_time_ns() noexcept;

}  // namespace arc
