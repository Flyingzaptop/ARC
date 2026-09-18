#include "arc/resource_semantics.hpp"

#include <algorithm>
#include <cmath>

namespace arc {
namespace {

constexpr std::uint64_t bit(unsigned i) noexcept { return 1ull << i; }

float clamp01(double x) noexcept {
    return static_cast<float>(std::clamp(x, 0.0, 1.0));
}

bool has_view(const ResourceRecord& r, ViewType type) noexcept {
    return (r.evidence & (1U << static_cast<unsigned>(type))) != 0;
}

} // namespace

ResourceSemanticFeatures ResourceSemanticInferencer::extract(
    const ResourceGraph& graph,
    ResourceId id) const noexcept
{
    ResourceSemanticFeatures f{};
    f.resource = id;
    const auto found = graph.find(id);
    if (!found) return f;

    const auto& r = *found;
    const auto& d = r.description;
    f.kind = d.kind;
    f.allocation_bytes = d.allocation_bytes;
    f.width = d.width;
    f.height = d.height;
    f.mip_levels = d.mip_levels;
    f.array_layers = d.array_layers;
    f.sample_count = d.sample_count;
    f.srv = has_view(r, ViewType::Srv);
    f.uav = has_view(r, ViewType::Uav);
    f.rtv = has_view(r, ViewType::Rtv);
    f.dsv = has_view(r, ViewType::Dsv);
    f.cbv = has_view(r, ViewType::Cbv);
    f.usage_count = r.read_count + r.write_count;
    if (f.usage_count) {
        f.read_fraction = static_cast<double>(r.read_count) / static_cast<double>(f.usage_count);
        f.write_fraction = static_cast<double>(r.write_count) / static_cast<double>(f.usage_count);
    }
    f.reuse_interval_frames = r.reuse_interval_frames;
    f.usage_bursts = r.usage_bursts;
    f.queue_count = r.queues.size();
    const auto frame = graph.presentation_frame();
    f.age_frames = frame >= r.last_used_frame ? frame - r.last_used_frame : 0;
    f.alive = r.alive;
    return f;
}

ResourceSemanticPrediction ResourceSemanticInferencer::classify(
    const ResourceSemanticFeatures& features) const noexcept
{
    ResourceSemanticPrediction out{};
    out.resource = features.resource;
    out.features = features;
    const auto& f = out.features;
    if (!f.resource || f.kind == ResourceKind::Unknown) return out;

    // Evidence bits are intentionally backend-neutral:
    // 0 SRV, 1 UAV, 2 RTV, 3 DSV, 4 texture, 5 buffer, 6 mip chain,
    // 7 mostly-read, 8 mostly-write, 9 transient/reused, 10 depth-like,
    // 11 large 2D target, 12 persistent/recurrent.
    if (f.srv) out.evidence_mask |= bit(0);
    if (f.uav) out.evidence_mask |= bit(1);
    if (f.rtv) out.evidence_mask |= bit(2);
    if (f.dsv) out.evidence_mask |= bit(3);

    const bool texture =
        f.kind == ResourceKind::Texture1D ||
        f.kind == ResourceKind::Texture2D ||
        f.kind == ResourceKind::Texture3D;
    const bool buffer = f.kind == ResourceKind::Buffer;
    if (texture) out.evidence_mask |= bit(4);
    if (buffer) out.evidence_mask |= bit(5);
    if (f.mip_levels > 1) out.evidence_mask |= bit(6);
    if (f.read_fraction >= 0.80) out.evidence_mask |= bit(7);
    if (f.write_fraction >= 0.65) out.evidence_mask |= bit(8);
    if (f.reuse_interval_frames > 0.0 && f.reuse_interval_frames <= 2.5 && f.usage_bursts >= 2)
        out.evidence_mask |= bit(9);
    if (f.dsv) out.evidence_mask |= bit(10);
    if (f.kind == ResourceKind::Texture2D && f.width >= 512 && f.height >= 512)
        out.evidence_mask |= bit(11);
    if (f.usage_bursts >= 4 || (f.reuse_interval_frames > 0.0 && f.reuse_interval_frames <= 3.0))
        out.evidence_mask |= bit(12);

    if (buffer) {
        if (f.cbv && f.write_fraction >= 0.50) {
            out.semantic = InferredResourceSemantic::UploadLikeBuffer;
            out.confidence = clamp01(0.55 + 0.35 * f.write_fraction);
            return out;
        }
        if (f.srv && !f.uav && f.read_fraction >= 0.85) {
            out.semantic = InferredResourceSemantic::GeometryBuffer;
            out.confidence = clamp01(0.58 + 0.30 * f.read_fraction);
            return out;
        }
        if (f.uav && f.write_fraction >= 0.70 && f.read_fraction < 0.20) {
            out.semantic = InferredResourceSemantic::ReadbackLikeBuffer;
            out.confidence = clamp01(0.45 + 0.35 * f.write_fraction);
            return out;
        }
        return out;
    }

    if (!texture) return out;

    if (f.dsv) {
        const bool shadow_shape =
            f.kind == ResourceKind::Texture2D &&
            f.width >= 256 && f.height >= 256 &&
            (f.width == f.height || f.array_layers > 1) &&
            !f.rtv &&
            (f.srv || f.read_fraction > 0.15);

        if (shadow_shape) {
            out.semantic = InferredResourceSemantic::ShadowMap;
            double score = 0.64;
            if (f.width == f.height) score += 0.08;
            if (f.array_layers > 1) score += 0.06;
            if (f.srv) score += 0.10;
            out.confidence = clamp01(score);
        } else {
            out.semantic = InferredResourceSemantic::DepthBuffer;
            out.confidence = clamp01(0.72 + (f.srv ? 0.08 : 0.0));
        }
        return out;
    }

    if (f.rtv) {
        if (f.srv && f.usage_bursts >= 3 && f.reuse_interval_frames > 0.0 && f.reuse_interval_frames <= 3.0) {
            out.semantic = InferredResourceSemantic::PersistentHistory;
            out.confidence = clamp01(0.58 + std::min(0.25, static_cast<double>(f.usage_bursts) / 20.0));
        } else if (f.uav || f.write_fraction >= 0.55) {
            out.semantic = InferredResourceSemantic::TransientIntermediate;
            out.confidence = clamp01(0.66 + 0.18 * f.write_fraction);
        } else {
            out.semantic = InferredResourceSemantic::RenderTarget;
            out.confidence = 0.78f;
        }
        return out;
    }

    if (f.uav) {
        out.semantic = InferredResourceSemantic::StorageTexture;
        out.confidence = clamp01(0.70 + 0.20 * f.write_fraction);
        return out;
    }

    if (f.srv && f.mip_levels > 1 && f.read_fraction >= 0.70) {
        out.semantic = InferredResourceSemantic::MaterialTexture;
        double score = 0.62 + 0.18 * f.read_fraction;
        if (f.width >= 256 && f.height >= 256) score += 0.08;
        out.confidence = clamp01(score);
        return out;
    }

    if (f.srv && f.usage_bursts >= 4 && f.reuse_interval_frames > 0.0 && f.reuse_interval_frames <= 3.0) {
        out.semantic = InferredResourceSemantic::PersistentHistory;
        out.confidence = 0.60f;
        return out;
    }

    return out;
}

ResourceSemanticPrediction ResourceSemanticInferencer::classify(
    const ResourceGraph& graph,
    ResourceId id) const noexcept
{
    return classify(extract(graph, id));
}

std::vector<ResourceSemanticPrediction> ResourceSemanticInferencer::classify_all(
    const ResourceGraph& graph,
    bool alive_only) const
{
    std::vector<ResourceSemanticPrediction> out;
    out.reserve(graph.resources().size());
    for (const auto& [id, record] : graph.resources()) {
        if (alive_only && !record.alive) continue;
        out.push_back(classify(graph, id));
    }
    return out;
}

} // namespace arc
