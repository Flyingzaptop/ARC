#include "arc/transition_predictor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace arc {
namespace {

bool valid_probability(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

}  // namespace

ResourceTransitionPredictor::ResourceTransitionPredictor(TransitionPredictorConfig config)
    : config_(config) {
    if (!config_.minimum_context_observations || !config_.max_context_samples || !config_.max_predictions ||
        config_.max_context_samples < config_.minimum_context_observations ||
        !valid_probability(config_.minimum_probability) || !valid_probability(config_.minimum_confidence)) {
        config_ = TransitionPredictorConfig{};
    }
}

std::size_t ResourceTransitionPredictor::PairHash::operator()(const PairKey& value) const noexcept {
    const auto h1 = std::hash<ResourceId>{}(value.first);
    const auto h2 = std::hash<ResourceId>{}(value.second);
    return h1 ^ (h2 + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (h1 << 6U) + (h1 >> 2U));
}

void ResourceTransitionPredictor::add(Counter& counter, ResourceId next) {
    if (!next) {
        return;
    }
    if (counter.total >= config_.max_context_samples) {
        std::uint32_t total{};
        for (auto it = counter.next.begin(); it != counter.next.end();) {
            it->second = (it->second + 1U) / 2U;
            if (!it->second) {
                it = counter.next.erase(it);
            } else {
                total += it->second;
                ++it;
            }
        }
        counter.total = total;
    }
    auto& count = counter.next[next];
    if (count != (std::numeric_limits<std::uint32_t>::max)()) {
        ++count;
    }
    if (counter.total != (std::numeric_limits<std::uint32_t>::max)()) {
        ++counter.total;
    }
}

void ResourceTransitionPredictor::observe(ResourceId resource) {
    if (!resource) {
        reset_context();
        return;
    }

    if (have_previous1_) {
        add(order1_[previous1_], resource);
    }
    if (have_previous2_) {
        add(order2_[PairKey{previous2_, previous1_}], resource);
    }

    previous2_ = previous1_;
    previous1_ = resource;
    have_previous2_ = have_previous1_;
    have_previous1_ = true;
    ++observations_;
}

void ResourceTransitionPredictor::reset_context() noexcept {
    previous2_ = 0;
    previous1_ = 0;
    have_previous1_ = false;
    have_previous2_ = false;
}

std::vector<TransitionPrediction> ResourceTransitionPredictor::from_counter(
    const Counter& counter,
    std::uint8_t order) const {
    if (counter.total < config_.minimum_context_observations) {
        return {};
    }

    std::vector<TransitionPrediction> result;
    result.reserve(counter.next.size());
    const auto sample_span = static_cast<double>(config_.minimum_context_observations) * 2.0;
    const auto support = (std::min)(1.0, static_cast<double>(counter.total) / sample_span);
    for (const auto& [resource, count] : counter.next) {
        const auto probability = static_cast<double>(count) / static_cast<double>(counter.total);
        const auto confidence = probability * support;
        if (probability < config_.minimum_probability || confidence < config_.minimum_confidence) {
            continue;
        }
        result.push_back(TransitionPrediction{resource, probability, confidence, count, order});
    }

    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.confidence != right.confidence) return left.confidence > right.confidence;
        if (left.probability != right.probability) return left.probability > right.probability;
        if (left.observations != right.observations) return left.observations > right.observations;
        return left.resource < right.resource;
    });
    if (result.size() > config_.max_predictions) {
        result.resize(config_.max_predictions);
    }
    return result;
}

std::vector<TransitionPrediction> ResourceTransitionPredictor::predictions() const {
    if (!have_previous1_) {
        return {};
    }

    if (have_previous2_) {
        const auto second = order2_.find(PairKey{previous2_, previous1_});
        if (second != order2_.end()) {
            auto result = from_counter(second->second, 2);
            if (!result.empty()) {
                return result;
            }
        }
    }

    const auto first = order1_.find(previous1_);
    return first == order1_.end() ? std::vector<TransitionPrediction>{} : from_counter(first->second, 1);
}

}  // namespace arc
