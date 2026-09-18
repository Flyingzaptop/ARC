#include "arc/scene_understanding.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace arc {
namespace {

float clamp01(double x) noexcept {
    return static_cast<float>(std::clamp(x, 0.0, 1.0));
}

float safe_ratio(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    if (!denominator) return 0.0F;
    return static_cast<float>(static_cast<double>(numerator) / static_cast<double>(denominator));
}

float normalized_log_distance(std::uint64_t a, std::uint64_t b, double span) noexcept {
    const double la = std::log1p(static_cast<double>(a));
    const double lb = std::log1p(static_cast<double>(b));
    return clamp01(std::abs(la - lb) / span);
}

template<std::size_t N>
float total_variation(const std::array<float, N>& a, const std::array<float, N>& b) noexcept {
    double sum = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        sum += std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
    }
    return clamp01(0.5 * sum);
}

void blend(float& dst, float src, float alpha) noexcept {
    dst += alpha * (src - dst);
}

void blend_signature(SceneSemanticSignature& dst, const SceneSemanticSignature& src, float alpha) noexcept {
    dst.frame = src.frame;
    blend(dst.coverage, src.coverage, alpha);
    blend(dst.mean_confidence, src.mean_confidence, alpha);
    blend(dst.read_fraction, src.read_fraction, alpha);
    blend(dst.write_fraction, src.write_fraction, alpha);
    blend(dst.multi_queue_fraction, src.multi_queue_fraction, alpha);

    const auto blend_count = [alpha](std::uint64_t current, std::uint64_t sample) noexcept {
        const double value = static_cast<double>(current) +
            static_cast<double>(alpha) * (static_cast<double>(sample) - static_cast<double>(current));
        return static_cast<std::uint64_t>(std::max(0.0, std::round(value)));
    };
    dst.active_resources = blend_count(dst.active_resources, src.active_resources);
    dst.known_resources = blend_count(dst.known_resources, src.known_resources);
    dst.active_bytes = blend_count(dst.active_bytes, src.active_bytes);
    dst.known_bytes = blend_count(dst.known_bytes, src.known_bytes);

    for (std::size_t i = 0; i < dst.resource_fractions.size(); ++i) {
        blend(dst.resource_fractions[i], src.resource_fractions[i], alpha);
        blend(dst.byte_fractions[i], src.byte_fractions[i], alpha);
    }
}

} // namespace

SceneUnderstandingInferencer::SceneUnderstandingInferencer(SceneUnderstandingConfig config) noexcept
    : config_(config)
{
    config_.minimum_resource_confidence = std::clamp(config_.minimum_resource_confidence, 0.0F, 1.0F);
    config_.same_scene_distance = std::clamp(config_.same_scene_distance, 0.01F, 1.0F);
    config_.cluster_distance = std::clamp(config_.cluster_distance, 0.01F, 1.0F);
    config_.centroid_learning_rate = std::clamp(config_.centroid_learning_rate, 0.01F, 1.0F);
}

SceneObservationCheckpoint SceneUnderstandingInferencer::checkpoint(const ResourceGraph& graph) const {
    SceneObservationCheckpoint out{};
    out.frame = graph.presentation_frame();
    out.valid = true;
    out.resources.reserve(graph.resources().size());
    for (const auto& [id, record] : graph.resources()) {
        if (!record.alive) continue;
        out.resources.emplace(id, SceneResourceUsageCheckpoint{
            record.read_count, record.write_count, record.usage_bursts});
    }
    return out;
}

SceneSemanticSignature SceneUnderstandingInferencer::summarize(const ResourceGraph& graph) const {
    SceneObservationCheckpoint empty{};
    return summarize(graph, empty);
}

SceneSemanticSignature SceneUnderstandingInferencer::summarize(
    const ResourceGraph& graph,
    const SceneObservationCheckpoint& checkpoint) const
{
    SceneSemanticSignature out{};
    out.frame = graph.presentation_frame();

    ResourceSemanticInferencer resources{};
    std::array<std::uint64_t, kInferredResourceSemanticCount> semantic_resources{};
    std::array<std::uint64_t, kInferredResourceSemanticCount> semantic_bytes{};
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t multi_queue = 0;
    double confidence_sum = 0.0;

    for (const auto& [id, record] : graph.resources()) {
        if (!record.alive) continue;

        auto features = resources.extract(graph, id);
        std::uint64_t delta_reads = record.read_count;
        std::uint64_t delta_writes = record.write_count;
        std::uint64_t delta_bursts = record.usage_bursts;

        if (checkpoint.valid) {
            std::uint64_t prior_reads = 0;
            std::uint64_t prior_writes = 0;
            std::uint64_t prior_bursts = 0;
            if (const auto it = checkpoint.resources.find(id); it != checkpoint.resources.end()) {
                prior_reads = it->second.reads;
                prior_writes = it->second.writes;
                prior_bursts = it->second.usage_bursts;
            }
            delta_reads = record.read_count >= prior_reads ? record.read_count - prior_reads : record.read_count;
            delta_writes = record.write_count >= prior_writes ? record.write_count - prior_writes : record.write_count;
            delta_bursts = record.usage_bursts >= prior_bursts ?
                record.usage_bursts - prior_bursts : record.usage_bursts;
        } else if (config_.active_window_frames > 0 &&
                   out.frame >= record.last_used_frame &&
                   out.frame - record.last_used_frame > config_.active_window_frames) {
            continue;
        }

        const std::uint64_t delta_usage = delta_reads + delta_writes;
        if (!delta_usage) continue;

        features.usage_count = delta_usage;
        features.usage_bursts = delta_bursts;
        features.read_fraction = static_cast<double>(delta_reads) / static_cast<double>(delta_usage);
        features.write_fraction = static_cast<double>(delta_writes) / static_cast<double>(delta_usage);
        const auto prediction = resources.classify(features);

        ++out.active_resources;
        out.active_bytes += features.allocation_bytes;
        reads += delta_reads;
        writes += delta_writes;
        if (features.queue_count > 1) ++multi_queue;

        if (prediction.semantic == InferredResourceSemantic::Unknown ||
            prediction.confidence < config_.minimum_resource_confidence) {
            continue;
        }

        const auto index = static_cast<std::size_t>(prediction.semantic);
        if (index >= kInferredResourceSemanticCount) continue;
        ++out.known_resources;
        out.known_bytes += features.allocation_bytes;
        ++semantic_resources[index];
        semantic_bytes[index] += features.allocation_bytes;
        confidence_sum += prediction.confidence;
    }

    out.coverage = safe_ratio(out.known_resources, out.active_resources);
    out.mean_confidence = out.known_resources ?
        static_cast<float>(confidence_sum / static_cast<double>(out.known_resources)) : 0.0F;
    const auto accesses = reads + writes;
    out.read_fraction = safe_ratio(reads, accesses);
    out.write_fraction = safe_ratio(writes, accesses);
    out.multi_queue_fraction = safe_ratio(multi_queue, out.active_resources);

    for (std::size_t i = 0; i < kInferredResourceSemanticCount; ++i) {
        out.resource_fractions[i] = safe_ratio(semantic_resources[i], out.known_resources);
        out.byte_fractions[i] = safe_ratio(semantic_bytes[i], out.known_bytes);
    }
    return out;
}

SceneSemanticComparison SceneUnderstandingInferencer::compare(
    const SceneSemanticSignature& a,
    const SceneSemanticSignature& b) const noexcept
{
    SceneSemanticComparison out{};
    if (!a.active_resources || !b.active_resources || !a.known_resources || !b.known_resources) {
        out.distance = 1.0F;
        out.similarity = 0.0F;
        out.confidence = 0.0F;
        out.same_scene = false;
        return out;
    }

    const float semantic_resource_distance = total_variation(a.resource_fractions, b.resource_fractions);
    const float semantic_byte_distance = total_variation(a.byte_fractions, b.byte_fractions);
    const float behavior_distance = static_cast<float>((
        std::abs(static_cast<double>(a.read_fraction) - static_cast<double>(b.read_fraction)) +
        std::abs(static_cast<double>(a.write_fraction) - static_cast<double>(b.write_fraction)) +
        std::abs(static_cast<double>(a.multi_queue_fraction) - static_cast<double>(b.multi_queue_fraction)) +
        std::abs(static_cast<double>(a.coverage) - static_cast<double>(b.coverage))) / 4.0);
    const float population_distance =
        0.55F * normalized_log_distance(a.active_resources, b.active_resources, 4.0) +
        0.45F * normalized_log_distance(a.active_bytes, b.active_bytes, 12.0);

    out.distance = clamp01(
        0.38 * semantic_resource_distance +
        0.27 * semantic_byte_distance +
        0.20 * behavior_distance +
        0.15 * population_distance);
    out.similarity = 1.0F - out.distance;

    const float evidence = std::min({a.coverage, b.coverage, a.mean_confidence, b.mean_confidence});
    const float margin = clamp01((config_.same_scene_distance - out.distance) /
        std::max(0.01F, config_.same_scene_distance));
    out.same_scene = out.distance <= config_.same_scene_distance;
    out.confidence = out.same_scene ?
        clamp01(0.55 * evidence + 0.45 * margin) :
        clamp01(0.55 * evidence + 0.45 * (1.0F - margin));
    return out;
}

SceneSemanticClusterer::SceneSemanticClusterer(SceneUnderstandingConfig config) noexcept
    : config_(config), inferencer_(config)
{
    config_.minimum_resource_confidence = inferencer_.config().minimum_resource_confidence;
    config_.same_scene_distance = inferencer_.config().same_scene_distance;
    config_.cluster_distance = inferencer_.config().cluster_distance;
    config_.centroid_learning_rate = inferencer_.config().centroid_learning_rate;
}

SceneClusterAssignment SceneSemanticClusterer::observe(const SceneSemanticSignature& signature) {
    SceneClusterAssignment out{};
    if (!signature.active_resources || !signature.known_resources) {
        out.distance = 1.0F;
        return out;
    }

    std::size_t best = centroids_.size();
    float best_distance = std::numeric_limits<float>::infinity();
    float best_confidence = 0.0F;
    for (std::size_t i = 0; i < centroids_.size(); ++i) {
        const auto comparison = inferencer_.compare(signature, centroids_[i]);
        if (comparison.distance < best_distance) {
            best = i;
            best_distance = comparison.distance;
            best_confidence = comparison.confidence;
        }
    }

    if (best == centroids_.size() || best_distance > config_.cluster_distance) {
        centroids_.push_back(signature);
        observations_.push_back(1);
        out.cluster = static_cast<std::uint32_t>(centroids_.size());
        out.distance = best == centroids_.size() ? 1.0F : best_distance;
        out.confidence = clamp01(std::min(signature.coverage, signature.mean_confidence));
        out.created = true;
        return out;
    }

    ++observations_[best];
    const float effective_alpha = std::max(
        0.04F,
        config_.centroid_learning_rate / std::sqrt(static_cast<float>(observations_[best])));
    blend_signature(centroids_[best], signature, effective_alpha);
    out.cluster = static_cast<std::uint32_t>(best + 1);
    out.distance = best_distance;
    out.confidence = best_confidence;
    out.created = false;
    return out;
}

void SceneSemanticClusterer::reset() noexcept {
    centroids_.clear();
    observations_.clear();
}

} // namespace arc
