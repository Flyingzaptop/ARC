#pragma once

#include "arc/ids.hpp"

#include <cstdint>
#include <vector>

namespace arc {

enum class MemoryActionKind : std::uint8_t {
    EvictResource,
    DemoteTexture,
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

class MemoryArbiter final {
public:
    explicit MemoryArbiter(MemoryArbiterConfig config = {});

    [[nodiscard]] MemoryArbiterPlan plan(
        std::uint64_t bytes_to_free,
        const std::vector<MemoryActionCandidate>& candidates) const;

    [[nodiscard]] const MemoryArbiterConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] double penalty(const MemoryActionCandidate& candidate) const noexcept;

    MemoryArbiterConfig config_{};
};

}  // namespace arc
