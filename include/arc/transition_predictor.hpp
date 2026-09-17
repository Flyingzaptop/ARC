#pragma once

#include "arc/ids.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace arc {

struct TransitionPredictorConfig {
    std::uint32_t minimum_context_observations{4};
    std::uint32_t max_context_samples{512};
    std::uint32_t max_predictions{4};
    double minimum_probability{0.10};
    double minimum_confidence{0.25};
};

struct TransitionPrediction {
    ResourceId resource{};
    double probability{};
    double confidence{};
    std::uint32_t observations{};
    std::uint8_t order{};

    friend bool operator==(const TransitionPrediction&, const TransitionPrediction&) = default;
};

// Bounded order-2 Markov predictor with order-1 backoff. It is deliberately
// backend-neutral and learns only resource-use sequence, not mutation policy.
class ResourceTransitionPredictor final {
public:
    explicit ResourceTransitionPredictor(TransitionPredictorConfig config = {});

    void observe(ResourceId resource);
    void reset_context() noexcept;

    [[nodiscard]] std::vector<TransitionPrediction> predictions() const;
    [[nodiscard]] std::uint64_t observations() const noexcept { return observations_; }
    [[nodiscard]] const TransitionPredictorConfig& config() const noexcept { return config_; }

private:
    struct PairKey {
        ResourceId first{};
        ResourceId second{};
        friend bool operator==(const PairKey&, const PairKey&) = default;
    };
    struct PairHash {
        [[nodiscard]] std::size_t operator()(const PairKey& value) const noexcept;
    };
    struct Counter {
        std::unordered_map<ResourceId, std::uint32_t> next{};
        std::uint32_t total{};
    };

    void add(Counter& counter, ResourceId next);
    [[nodiscard]] std::vector<TransitionPrediction> from_counter(const Counter& counter, std::uint8_t order) const;

    TransitionPredictorConfig config_{};
    std::unordered_map<ResourceId, Counter> order1_{};
    std::unordered_map<PairKey, Counter, PairHash> order2_{};
    ResourceId previous2_{};
    ResourceId previous1_{};
    bool have_previous1_{};
    bool have_previous2_{};
    std::uint64_t observations_{};
};

}  // namespace arc
