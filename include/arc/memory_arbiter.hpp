#pragma once

#include "arc/ids.hpp"

#include <cstdint>
#include <vector>

namespace arc {

enum class MemoryActionKind : std::uint8_t {
    EvictResource,
    DemoteTexture,
};

enum class MemoryRestoreKind : std::uint8_t {
    MakeResident,
    PromoteTexture,
};

// A backend-neutral candidate produced by a specialized policy. sequence is
// used for ordered texture demotions: step N+1 cannot be selected before N.
struct MemoryActionCandidate {
    MemoryActionKind kind{MemoryActionKind::EvictResource};
    ResourceId resource{};
    std::uint64_t subject{};
    std::uint32_t sequence{};
    std::uint64_t bytes_freed{};
    double quality_loss{};
    double latency_risk_ms{};
    double confidence{1.0};
    bool eligible{true};
};

// Restoration actions consume headroom. sequence is the execution order for
// multiple promotions of the same texture (0 first, then 1, ...).
struct MemoryRestoreCandidate {
    MemoryRestoreKind kind{MemoryRestoreKind::MakeResident};
    ResourceId resource{};
    std::uint64_t subject{};
    std::uint32_t sequence{};
    std::uint64_t bytes_cost{};
    double quality_gain{};
    double latency_benefit_ms{};
    double confidence{1.0};
    bool eligible{true};
};

struct MemoryArbiterConfig {
    double quality_weight{1.0};
    double latency_weight{1.0};
    double uncertainty_weight{0.10};
    std::uint32_t max_actions{128};
};

struct MemoryArbiterAction {
    MemoryActionCandidate candidate{};
    double total_penalty{};
    double penalty_per_byte{};
};

struct MemoryArbiterPlan {
    std::uint64_t requested_bytes{};
    std::uint64_t planned_bytes{};
    double total_penalty{};
    bool shortfall{};
    std::vector<MemoryArbiterAction> actions{};
};

struct MemoryRestoreAction {
    MemoryRestoreCandidate candidate{};
    double total_benefit{};
    double benefit_per_byte{};
};

struct MemoryRestorePlan {
    std::uint64_t headroom_bytes{};
    std::uint64_t planned_bytes{};
    double total_benefit{};
    std::vector<MemoryRestoreAction> actions{};
};

class MemoryArbiter final {
public:
    explicit MemoryArbiter(MemoryArbiterConfig config = {});

    [[nodiscard]] MemoryArbiterPlan plan(
        std::uint64_t bytes_to_free,
        const std::vector<MemoryActionCandidate>& candidates) const;

    [[nodiscard]] MemoryRestorePlan plan_restore(
        std::uint64_t headroom_bytes,
        const std::vector<MemoryRestoreCandidate>& candidates) const;

    [[nodiscard]] const MemoryArbiterConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] double penalty(const MemoryActionCandidate& candidate) const noexcept;
    [[nodiscard]] double benefit(const MemoryRestoreCandidate& candidate) const noexcept;

    MemoryArbiterConfig config_{};
};

}  // namespace arc
