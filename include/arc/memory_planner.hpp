#pragma once

#include "arc/memory_arbiter.hpp"
#include "arc/residency.hpp"
#include "arc/texture_quality.hpp"

#include <cstdint>

namespace arc {

struct GlobalMemoryPlannerConfig {
    MemoryArbiterConfig arbiter{};
    double unknown_prediction_confidence{0.50};
    double minimum_reload_risk_factor{0.10};
};

struct GlobalMemoryPlan {
    std::uint64_t requested_bytes{};
    std::uint64_t residency_candidate_bytes{};
    std::uint64_t texture_candidate_bytes{};
    MemoryArbiterPlan arbitration{};
};

struct GlobalMemoryRestorePlan {
    std::uint64_t headroom_bytes{};
    std::uint64_t residency_candidate_bytes{};
    std::uint64_t texture_candidate_bytes{};
    MemoryRestorePlan arbitration{};
};

// Slow-loop planner only. Specialized governors remain authoritative for
// safety; this class compares already-safe actions and never performs them.
class GlobalMemoryPlanner final {
public:
    explicit GlobalMemoryPlanner(GlobalMemoryPlannerConfig config = {});

    [[nodiscard]] GlobalMemoryPlan plan_pressure_relief(
        const ResidencyGovernor& residency,
        const TextureQualityGovernor& textures,
        std::uint64_t epoch) const;

    [[nodiscard]] GlobalMemoryRestorePlan plan_headroom_restore(
        const ResidencyGovernor& residency,
        const TextureQualityGovernor& textures,
        std::uint64_t epoch,
        std::uint64_t headroom_bytes) const;

    [[nodiscard]] const GlobalMemoryPlannerConfig& config() const noexcept { return config_; }

private:
    GlobalMemoryPlannerConfig config_{};
    MemoryArbiter arbiter_{};
};

}  // namespace arc
