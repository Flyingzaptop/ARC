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

float safe_rate(std::uint64_t value, FrameId frames) noexcept {
    if (!frames) return 0.0F;
    return static_cast<float>(static_cast<double>(value) / static_cast<double>(frames));
}

float normalized_log_distance(double a, double b, double span) noexcept {
    const double la = std::log1p(std::max(0.0, a));
    const double lb = std::log1p(std::max(0.0, b));
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
    dst.window_frames = src.window_frames;
    blend(dst.coverage, src.coverage, alpha);
    blend(dst.mean_confidence, src.mean_confidence, alpha);
    blend(dst.read_fraction, src.read_fraction, alpha);
    blend(dst.write_fraction, src.write_fraction, alpha);
    blend(dst.multi_queue_fraction, src.multi_queue_fraction, alpha);
    blend(dst.resource_accesses_per_frame, src.resource_accesses_per_frame, alpha);
    blend(dst.draw_calls_per_frame, src.draw_calls_per_frame, alpha);
    blend(dst.draw_items_per_frame, src.draw_items_per_frame, alpha);
    blend(dst.dispatches_per_frame, src.dispatches_per_frame, alpha);
    blend(dst.dispatch_groups_per_frame, src.dispatch_groups_per_frame, alpha);
    blend(dst.indirect_per_frame, src.indirect_per_frame, alpha);
    blend(dst.submissions_per_frame, src.submissions_per_frame, alpha);
    blend(dst.copies_per_frame, src.copies_per_frame, alpha);

    const auto blend_count = [alpha](std::uint64_t current, std::uint64_t sample) noexcept {
        const double value = static_cast<double>(current) +
            static_cast<double>(alpha) * (static_cast<double>(sample) - static_cast<double>(current));
        return static_cast<std::uint64_t>(std::max(0.0, std::round(value)));
    };
    dst.active_resources = blend_count(dst.active_resources, src.active_resources);
    dst.known_resources = blend_count(dst.known_resources, src.known_resources);
    dst.active_bytes = blend_count(dst.active_bytes, src.active_bytes);
    dst.known_bytes = blend_count(dst.known_bytes, src.known_bytes);
    dst.submissions = blend_count(dst.submissions, src.submissions);
    dst.draws = blend_count(dst.draws, src.draws);
    dst.indexed_draws = blend_count(dst.indexed_draws, src.indexed_draws);
    dst.dispatches = blend_count(dst.dispatches, src.dispatches);
    dst.indirect = blend_count(dst.indirect, src.indirect);
    dst.draw_items = blend_count(dst.draw_items, src.draw_items);
    dst.dispatch_groups = blend_count(dst.dispatch_groups, src.dispatch_groups);
    dst.copies = blend_count(dst.copies, src.copies);

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
    out.submission_count = graph.submissions().size();
    out.copy_count = graph.copies().size();
    out.valid = true;
    out.resources.reserve(graph.resources().size());
    for (const auto& [id, record] : graph.resources()) {
        if (!record.alive) continue;
        SceneResourceUsageCheckpoint usage{};
        usage.reads = record.read_count;
        usage.writes = record.write_count;
        usage.usage_bursts = record.usage_bursts;
        usage.reuse_gap_frames_sum = record.reuse_gap_frames_sum;
        usage.reuse_gap_samples = record.reuse_gap_samples;
        usage.queue_use_counts = record.queue_use_counts;
        out.resources.emplace(id, usage);
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
    if (checkpoint.valid) {
        out.window_frames = out.frame >= checkpoint.frame ?
            std::max<FrameId>(1, out.frame - checkpoint.frame) : 1;
    } else if (config_.active_window_frames > 0) {
        out.window_frames = std::max<FrameId>(
            1, std::min(config_.active_window_frames, std::max<FrameId>(1, out.frame)));
    } else {
        out.window_frames = std::max<FrameId>(1, out.frame);
    }

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
            std::uint64_t prior_gap_sum = 0;
            std::uint64_t prior_gap_samples = 0;
            const SceneResourceUsageCheckpoint* prior = nullptr;
            if (const auto it = checkpoint.resources.find(id); it != checkpoint.resources.end()) {
                prior = &it->second;
                prior_reads = prior->reads;
                prior_writes = prior->writes;
                prior_bursts = prior->usage_bursts;
                prior_gap_sum = prior->reuse_gap_frames_sum;
                prior_gap_samples = prior->reuse_gap_samples;
            }
            delta_reads = record.read_count >= prior_reads ? record.read_count - prior_reads : record.read_count;
            delta_writes = record.write_count >= prior_writes ? record.write_count - prior_writes : record.write_count;
            delta_bursts = record.usage_bursts >= prior_bursts ?
                record.usage_bursts - prior_bursts : record.usage_bursts;

            const auto gap_sum = record.reuse_gap_frames_sum >= prior_gap_sum ?
                record.reuse_gap_frames_sum - prior_gap_sum : record.reuse_gap_frames_sum;
            const auto gap_samples = record.reuse_gap_samples >= prior_gap_samples ?
                record.reuse_gap_samples - prior_gap_samples : record.reuse_gap_samples;
            features.reuse_interval_frames = gap_samples ?
                static_cast<double>(gap_sum) / static_cast<double>(gap_samples) : 0.0;

            std::size_t active_queues = 0;
            for (const auto& [queue, count] : record.queue_use_counts) {
                std::uint64_t prior_count = 0;
                if (prior) {
                    if (const auto q = prior->queue_use_counts.find(queue); q != prior->queue_use_counts.end())
                        prior_count = q->second;
                }
                if (count > prior_count) ++active_queues;
            }
            features.queue_count = active_queues;
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

    const auto& submissions = graph.submissions();
    const std::size_t submission_begin = checkpoint.valid ?
        std::min(checkpoint.submission_count, submissions.size()) : 0;
    for (std::size_t i = submission_begin; i < submissions.size(); ++i) {
        const auto& submission = submissions[i];
        if (!checkpoint.valid && config_.active_window_frames > 0 &&
            out.frame >= submission.presentation &&
            out.frame - submission.presentation > config_.active_window_frames) {
            continue;
        }
        ++out.submissions;
        out.draws += submission.counters.draws;
        out.indexed_draws += submission.counters.indexed_draws;
        out.dispatches += submission.counters.dispatches;
        out.indirect += submission.counters.indirect;
        out.draw_items += submission.counters.draw_items;
        out.dispatch_groups += submission.counters.dispatch_groups;
    }

    const auto& copies = graph.copies();
    const std::size_t copy_begin = checkpoint.valid ?
        std::min(checkpoint.copy_count, copies.size()) : 0;
    out.copies = static_cast<std::uint64_t>(copies.size() - copy_begin);

    out.coverage = safe_ratio(out.known_resources, out.active_resources);
    out.mean_confidence = out.known_resources ?
        static_cast<float>(confidence_sum / static_cast<double>(out.known_resources)) : 0.0F;
    const auto accesses = reads + writes;
    out.read_fraction = safe_ratio(reads, accesses);
    out.write_fraction = safe_ratio(writes, accesses);
    out.multi_queue_fraction = safe_ratio(multi_queue, out.active_resources);
    out.resource_accesses_per_frame = safe_rate(accesses, out.window_frames);
    out.draw_calls_per_frame = safe_rate(out.draws + out.indexed_draws, out.window_frames);
    out.draw_items_per_frame = safe_rate(out.draw_items, out.window_frames);
    out.dispatches_per_frame = safe_rate(out.dispatches, out.window_frames);
    out.dispatch_groups_per_frame = safe_rate(out.dispatch_groups, out.window_frames);
    out.indirect_per_frame = safe_rate(out.indirect, out.window_frames);
    out.submissions_per_frame = safe_rate(out.submissions, out.window_frames);
    out.copies_per_frame = safe_rate(out.copies, out.window_frames);

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
        0.55F * normalized_log_distance(
            static_cast<double>(a.active_resources), static_cast<double>(b.active_resources), 4.0) +
        0.45F * normalized_log_distance(
            static_cast<double>(a.active_bytes), static_cast<double>(b.active_bytes), 12.0);
    const std::array<float, 8> workload_axes{
        normalized_log_distance(a.resource_accesses_per_frame, b.resource_accesses_per_frame, 5.0),
        normalized_log_distance(a.draw_calls_per_frame, b.draw_calls_per_frame, 5.0),
        normalized_log_distance(a.draw_items_per_frame, b.draw_items_per_frame, 5.0),
        normalized_log_distance(a.dispatches_per_frame, b.dispatches_per_frame, 5.0),
        normalized_log_distance(a.dispatch_groups_per_frame, b.dispatch_groups_per_frame, 5.0),
        normalized_log_distance(a.indirect_per_frame, b.indirect_per_frame, 4.0),
        normalized_log_distance(a.submissions_per_frame, b.submissions_per_frame, 4.0),
        normalized_log_distance(a.copies_per_frame, b.copies_per_frame, 5.0),
    };
    double workload_mean = 0.0;
    for (const float axis : workload_axes) workload_mean += axis;
    workload_mean /= static_cast<double>(workload_axes.size());
    const float workload_distance = clamp01(
        0.70 * *std::max_element(workload_axes.begin(), workload_axes.end()) +
        0.30 * workload_mean);

    out.distance = clamp01(
        0.22 * semantic_resource_distance +
        0.18 * semantic_byte_distance +
        0.12 * behavior_distance +
        0.10 * population_distance +
        0.38 * workload_distance);
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

    const bool no_existing_cluster = best == centroids_.size();
    if (no_existing_cluster || best_distance > config_.cluster_distance) {
        centroids_.push_back(signature);
        observations_.push_back(1);
        out.cluster = static_cast<std::uint32_t>(centroids_.size());
        out.distance = no_existing_cluster ? 1.0F : best_distance;
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
