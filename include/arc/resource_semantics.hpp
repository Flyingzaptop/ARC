#pragma once

#include "arc/resource_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace arc {

enum class InferredResourceSemantic : std::uint8_t {
    Unknown,
    MaterialTexture,
    RenderTarget,
    DepthBuffer,
    ShadowMap,
    StorageTexture,
    StorageBuffer,
    GeometryBuffer,
    UploadLikeBuffer,
    ReadbackLikeBuffer,
    TransientIntermediate,
    PersistentHistory,
};

inline constexpr std::size_t kInferredResourceSemanticCount = 12;

// Scores rank heuristic support; they are not calibrated correctness probabilities.
enum class SemanticConfidenceBasis : std::uint8_t { HeuristicScore };

struct ResourceSemanticFeatures {
    ResourceId resource{};
    ResourceKind kind{ResourceKind::Unknown};
    std::uint64_t allocation_bytes{};
    std::uint64_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};
    std::uint32_t format{};
    std::uint32_t resource_flags{};
    std::uint16_t mip_levels{};
    std::uint16_t array_layers{};
    std::uint32_t sample_count{1};
    std::uint8_t plane_count{1};
    ResourceAllocationKind allocation_kind{ResourceAllocationKind::Committed};
    bool srv{};
    bool uav{};
    bool rtv{};
    bool dsv{};
    bool cbv{};
    double read_fraction{};
    double write_fraction{};
    double reuse_interval_frames{};
    std::uint64_t usage_count{};
    std::uint64_t usage_bursts{};
    std::size_t queue_count{};
    FrameId age_frames{};
    bool alive{};
};

struct ResourceSemanticPrediction {
    ResourceId resource{};
    InferredResourceSemantic semantic{InferredResourceSemantic::Unknown};
    float confidence{};
    SemanticConfidenceBasis confidence_basis{SemanticConfidenceBasis::HeuristicScore};
    std::uint64_t evidence_mask{};
    ResourceSemanticFeatures features{};
};

class ResourceSemanticInferencer final {
public:
    [[nodiscard]] ResourceSemanticFeatures extract(
        const ResourceGraph& graph,
        ResourceId resource) const noexcept;

    [[nodiscard]] ResourceSemanticPrediction classify(
        const ResourceSemanticFeatures& features) const noexcept;

    [[nodiscard]] ResourceSemanticPrediction classify(
        const ResourceGraph& graph,
        ResourceId resource) const noexcept;

    [[nodiscard]] std::vector<ResourceSemanticPrediction> classify_all(
        const ResourceGraph& graph,
        bool alive_only = true) const;
};

} // namespace arc
