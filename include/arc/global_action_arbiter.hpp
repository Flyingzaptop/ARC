#pragma once

#include "arc/adaptive_quality_controller.hpp"
#include "arc/live_runtime.hpp"

#include <cstdint>

namespace arc {

enum class GlobalArbitrationChoice : std::uint8_t {
    None,
    Memory,
    Quality,
    Combined,
    Restore,
};

struct GlobalActionArbiterConfig {
    double memory_pressure_enter{0.90};
    double memory_pressure_emergency{0.97};
    double visual_cost_floor{0.01};
    bool allow_combined{true};
};

struct GlobalArbitrationDecision {
    GlobalArbitrationChoice choice{GlobalArbitrationChoice::None};
    bool execute_memory{};
    bool execute_quality{};
    bool memory_has_work{};
    bool quality_has_work{};
    double memory_score{};
    double quality_score{};
    bool memory_emergency{};
};

// Cross-domain arbitration happens after specialized policies have already
// declared actions safe. It never invents mutations; it only chooses which
// already-safe plan(s) are allowed to execute on this control tick.
class GlobalActionArbiter final {
public:
    explicit GlobalActionArbiter(GlobalActionArbiterConfig config = {});

    [[nodiscard]] GlobalArbitrationDecision decide(
        const LiveRuntimePlan& memory,
        const QualityDecision& quality,
        const FrameBudgetSample& frame) const noexcept;

private:
    [[nodiscard]] bool memory_has_work(const LiveRuntimePlan& plan) const noexcept;
    [[nodiscard]] double memory_score(const LiveRuntimePlan& plan, double pressure) const noexcept;
    [[nodiscard]] double quality_score(const QualityDecision& quality, const FrameBudgetSample& frame) const noexcept;

    GlobalActionArbiterConfig config_{};
};

} // namespace arc
