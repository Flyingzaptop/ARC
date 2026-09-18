#include "arc/scene_understanding.hpp"

#include <algorithm>
#include <cmath>

namespace arc {
namespace {

constexpr std::size_t idx(ResourceSemantic x) noexcept {
    return static_cast<std::size_t>(x);
}

bool has_view(const ResourceRecord& r, ViewType type) noexcept {
    return (r.evidence & (1U << static_cast<unsigned>(type))) != 0;
}

double sat(double x) noexcept {
    return std::clamp(x, 0.0, 1.0);
}

double ratio(std::uint64_t a, std::uint64_t b) noexcept {
    const double total = static_cast<double>(a + b);
    return total > 0.0 ? static_cast<double>(a) / total : 0.0;
}

} // namespace

ResourceSemanticEstimate SceneUnderstandingModel::infer_resource(
    const ResourceRecord& r,
    const FrameId current_frame) noexcept {
    ResourceSemanticEstimate out{};
    out.resource = r.description.resource;

    const bool srv = has_view(r, ViewType::Srv);
    const bool uav = has_view(r, ViewType::Uav);
    const bool rtv = has_view(r, ViewType::Rtv);
    const bool dsv = has_view(r, ViewType::Dsv);
    const bool cbv = has_view(r, ViewType::Cbv);
    const bool texture = r.description.kind == ResourceKind::Texture1D ||
        r.description.kind == ResourceKind::Texture2D ||
        r.description.kind == ResourceKind::Texture3D;
    const bool texture2d = r.description.kind == ResourceKind::Texture2D;
    const bool buffer = r.description.kind == ResourceKind::Buffer;
    const double read_share = ratio(r.read_count, r.write_count);
    const double write_share = ratio(r.write_count, r.read_count);
    const auto age = current_frame >= r.last_used_frame ? current_frame - r.last_used_frame : 0;
    const double recent = (r.read_count + r.write_count) == 0 ? 0.0 : 1.0 / (1.0 + static_cast<double>(age));
    const double square = r.description.height > 0
        ? 1.0 - std::min(1.0, std::abs(
            std::log2(std::max(1.0, static_cast<double>(r.description.width) /
                                      static_cast<double>(r.description.height)))))
        : 0.0;
    const double reused_every_frame = r.reuse_interval_frames > 0.0
        ? 1.0 / (1.0 + std::abs(r.reuse_interval_frames - 1.0))
        : 0.0;
    const double many_mips = r.description.mip_levels > 1 ? 1.0 : 0.0;

    auto& s = out.scores;

    if (texture && srv) {
        s[idx(ResourceSemantic::MaterialTexture)] += 0.30;
        s[idx(ResourceSemantic::MaterialTexture)] += 0.25 * many_mips;
        s[idx(ResourceSemantic::MaterialTexture)] += 0.25 * read_share;
        if (!rtv && !dsv && !uav) s[idx(ResourceSemantic::MaterialTexture)] += 0.20;
    }

    if (texture2d && dsv) {
        s[idx(ResourceSemantic::DepthTarget)] += 0.60;
        s[idx(ResourceSemantic::DepthTarget)] += 0.15 * write_share;
        if (!rtv && !uav) s[idx(ResourceSemantic::DepthTarget)] += 0.10;

        if (srv) {
            s[idx(ResourceSemantic::ShadowMap)] += 0.42;
            s[idx(ResourceSemantic::ShadowMap)] += 0.18 * square;
            s[idx(ResourceSemantic::ShadowMap)] += 0.20 * read_share;
            if (r.description.array_layers > 1) s[idx(ResourceSemantic::ShadowMap)] += 0.12;
            if (r.description.width <= 8192 && r.description.height <= 8192)
                s[idx(ResourceSemantic::ShadowMap)] += 0.08;
        }
    }

    if (texture && rtv) {
        s[idx(ResourceSemantic::ColorTarget)] += 0.52;
        s[idx(ResourceSemantic::ColorTarget)] += 0.18 * write_share;
        s[idx(ResourceSemantic::ColorTarget)] += 0.10 * recent;
    }

    if (texture && uav) {
        s[idx(ResourceSemantic::StorageTexture)] += 0.58;
        s[idx(ResourceSemantic::StorageTexture)] += 0.20 * write_share;
        s[idx(ResourceSemantic::StorageTexture)] += 0.10 * recent;
    }

    if (texture && srv && (rtv || uav) && r.write_count > 0 && r.read_count > 0) {
        s[idx(ResourceSemantic::TransientTarget)] += 0.34;
        s[idx(ResourceSemantic::TransientTarget)] += 0.24 * reused_every_frame;
        s[idx(ResourceSemantic::TransientTarget)] += 0.18 * recent;
        if (r.reuse_interval_frames > 0.0 && r.reuse_interval_frames <= 2.5)
            s[idx(ResourceSemantic::TransientTarget)] += 0.14;

        s[idx(ResourceSemantic::HistoryTexture)] += 0.30;
        s[idx(ResourceSemantic::HistoryTexture)] += 0.30 * reused_every_frame;
        s[idx(ResourceSemantic::HistoryTexture)] += 0.18 * read_share;
        if (r.usage_bursts <= 2) s[idx(ResourceSemantic::HistoryTexture)] += 0.10;
    }

    if (buffer && cbv) {
        s[idx(ResourceSemantic::ConstantBuffer)] += 0.78;
        s[idx(ResourceSemantic::ConstantBuffer)] += 0.12 * read_share;
    }

    if (buffer && !cbv && !uav && r.read_count > 0) {
        s[idx(ResourceSemantic::GeometryBuffer)] += 0.36;
        s[idx(ResourceSemantic::GeometryBuffer)] += 0.32 * read_share;
        if (!srv) s[idx(ResourceSemantic::GeometryBuffer)] += 0.10;
        if (r.write_count == 0) s[idx(ResourceSemantic::GeometryBuffer)] += 0.10;
    }

    if (!r.alive) {
        for (auto& x : s) x *= 0.75;
    }

    std::size_t best = 0;
    double best_score = 0.0;
    double second = 0.0;
    for (std::size_t i = 1; i < s.size(); ++i) {
        s[i] = sat(s[i]);
        if (s[i] > best_score) {
            second = best_score;
            best_score = s[i];
            best = i;
        } else {
            second = std::max(second, s[i]);
        }
    }

    if (best_score < 0.45) {
        out.semantic = ResourceSemantic::Unknown;
        out.confidence = sat(best_score * 0.8);
        return out;
    }

    out.semantic = static_cast<ResourceSemantic>(best);
    const double margin = std::max(0.0, best_score - second);
    out.confidence = sat(0.70 * best_score + 0.30 * margin);
    return out;
}

std::vector<ResourceSemanticEstimate> SceneUnderstandingModel::infer_all(
    const ResourceGraph& graph) {
    std::vector<ResourceSemanticEstimate> out;
    out.reserve(graph.resources().size());
    const auto frame = graph.presentation_frame();
    for (const auto& [_, resource] : graph.resources()) {
        if (!resource.alive) continue;
        out.push_back(infer_resource(resource, frame));
    }
    return out;
}

double SceneUnderstandingModel::semantic_coverage(
    const std::vector<ResourceSemanticEstimate>& estimates,
    const double minimum_confidence) noexcept {
    if (estimates.empty()) return 0.0;
    std::size_t covered = 0;
    for (const auto& estimate : estimates) {
        if (estimate.semantic != ResourceSemantic::Unknown &&
            estimate.confidence >= minimum_confidence) {
            ++covered;
        }
    }
    return static_cast<double>(covered) / static_cast<double>(estimates.size());
}

WorkloadSignature SceneUnderstandingModel::infer_workload(
    const WorkloadTelemetrySample& sample) noexcept {
    WorkloadSignature out{};
    const double frames = static_cast<double>(std::max<std::uint64_t>(1, sample.frames));
    const double draws = static_cast<double>(sample.draws + sample.indexed_draws) / frames;
    const double dispatches = static_cast<double>(sample.dispatches) / frames;
    const double uses = static_cast<double>(sample.resource_uses) / frames;
    const double descriptors = static_cast<double>(sample.descriptor_writes) / frames;
    const double barriers = static_cast<double>(sample.barriers) / frames;
    const double copies = static_cast<double>(sample.copies) / frames;

    out.geometry = sat(draws / 420.0 + static_cast<double>(sample.indirect) / frames / 120.0);
    out.compute = sat(dispatches / 180.0);
    out.bandwidth = sat(uses / 800.0 + descriptors / 80.0 + copies / 180.0);
    out.raster = sat(draws / 300.0 + uses / 1600.0);
    out.shadow = sat(draws / 850.0 + barriers / 140.0);
    out.activity = sat((draws + dispatches * 1.5 + uses * 0.12) / 700.0);

    const std::array<double, 5> scores{
        out.raster, out.geometry, out.compute, out.bandwidth, out.shadow
    };
    std::size_t best = 0;
    double best_score = scores[0];
    double second = 0.0;
    for (std::size_t i = 1; i < scores.size(); ++i) {
        if (scores[i] > best_score) {
            second = best_score;
            best_score = scores[i];
            best = i;
        } else {
            second = std::max(second, scores[i]);
        }
    }

    if (best_score < 0.20) {
        out.dominant = WorkloadClass::Balanced;
        out.confidence = sat(1.0 - best_score);
    } else if (best_score - second < 0.10 && second > 0.35) {
        out.dominant = WorkloadClass::Mixed;
        out.confidence = sat((best_score + second) * 0.5);
    } else {
        constexpr std::array<WorkloadClass, 5> classes{
            WorkloadClass::RasterHeavy,
            WorkloadClass::GeometryHeavy,
            WorkloadClass::ComputeHeavy,
            WorkloadClass::BandwidthHeavy,
            WorkloadClass::ShadowHeavy,
        };
        out.dominant = classes[best];
        out.confidence = sat(0.65 * best_score + 0.35 * (best_score - second));
    }

    if (sample.target_ms > 0.0 && sample.gpu_ms > 0.0) {
        out.confidence = sat(out.confidence *
            std::min(1.0, sample.gpu_ms / std::max(0.05, sample.target_ms)));
    }
    return out;
}

FrameBudgetSample SceneUnderstandingModel::enrich_frame(
    FrameBudgetSample frame,
    const WorkloadSignature& signature) noexcept {
    const auto blend = [](double current, double inferred) noexcept {
        if (current <= 0.0) return inferred;
        return std::clamp(0.65 * current + 0.35 * inferred, 0.0, 1.0);
    };
    frame.raster_pressure = blend(frame.raster_pressure, signature.raster);
    frame.geometry_pressure = blend(frame.geometry_pressure, signature.geometry);
    frame.lighting_pressure = blend(frame.lighting_pressure, signature.compute);
    frame.memory_bandwidth_fraction = blend(
        frame.memory_bandwidth_fraction, signature.bandwidth);
    frame.shadow_pressure = blend(frame.shadow_pressure, signature.shadow);
    return frame;
}

} // namespace arc
