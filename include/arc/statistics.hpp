#pragma once
#include <algorithm>
#include <vector>
#include <numeric>
#include <stdexcept>
namespace arc {
struct Statistics { double median{}, p10{}, p90{}, p95{}, p99{}, variance{}, mean{}; };
inline Statistics summarize(std::vector<double> values) {
    if (values.empty()) { throw std::invalid_argument("empty measurement set"); }
    const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    double variance{}; for (auto v : values) { variance += (v - mean) * (v - mean); }
    std::sort(values.begin(), values.end());
    auto q = [&](double p) { return values[static_cast<std::size_t>(p * (values.size() - 1))]; };
    return {q(.5), q(.1), q(.9), q(.95), q(.99), variance / values.size(), mean};
}
}
