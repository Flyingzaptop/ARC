#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace arc {
// Complete settings for one generation of an observed compute pipeline.
// No engine labels or shader-name/hash allowlists participate in admission.
struct ComputePolicy {
    std::uint64_t pipeline{};
    std::uint32_t x_rate{1}, y_rate{1}, comparison_taps{}, zero_factor{}, mip_steps{};
    bool protect_edges{};
    float edge_threshold{.08f};
    std::uint32_t sample_percent{100};
    bool operator==(const ComputePolicy&) const = default;
};
struct PolicyBundle {
    static constexpr std::size_t capacity = 8;
    std::uint64_t id{};
    std::vector<ComputePolicy> compute;
    bool cpu_state_cache{};
    [[nodiscard]] bool valid() const noexcept {
        if(compute.size()>capacity || (!id && (!compute.empty()||cpu_state_cache))) return false;
        for(std::size_t i=0;i<compute.size();++i) {
            const auto& p=compute[i];
            if(!p.pipeline || (p.x_rate!=1&&p.x_rate!=2&&p.x_rate!=4) || (p.y_rate!=1&&p.y_rate!=2&&p.y_rate!=4) ||
               (p.comparison_taps!=0&&p.comparison_taps!=9) || p.zero_factor>1 ||
               (p.mip_steps>8) || (p.sample_percent!=100&&p.sample_percent!=75&&p.sample_percent!=50&&p.sample_percent!=25) || (p.protect_edges&&p.sample_percent!=100) ||
               !std::isfinite(p.edge_threshold)||p.edge_threshold<0||p.edge_threshold>2) return false;
            for(std::size_t j=0;j<i;++j) if(compute[j].pipeline==p.pipeline) return false;
        }
        return true;
    }
    [[nodiscard]] const ComputePolicy* find(std::uint64_t pipeline) const noexcept {
        for(const auto& p:compute) if(p.pipeline==pipeline) return &p;
        return nullptr;
    }
    // Replace one target while preserving all other accepted settings. Caller
    // gives the resulting complete configuration its own identity/evidence.
    bool replace(ComputePolicy value) {
        for(auto& p:compute) if(p.pipeline==value.pipeline) {p=value;return true;}
        if(compute.size()==capacity) return false;
        compute.push_back(value);return true;
    }
};
}
