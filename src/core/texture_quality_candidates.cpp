#include "arc/texture_quality.hpp"

#include <algorithm>
#include <limits>

namespace arc {
namespace {

double demotion_candidate_score(const TextureQualityObject& object, const TextureDemotionStep& step) noexcept {
    return step.bytes_freed == 0
        ? std::numeric_limits<double>::infinity()
        : (step.quality_loss * object.importance) / static_cast<double>(step.bytes_freed);
}

double promotion_candidate_score(const TextureQualityObject& object, const TextureDemotionStep& step) noexcept {
    return step.bytes_freed == 0
        ? 0.0
        : (step.quality_loss * object.importance) / static_cast<double>(step.bytes_freed);
}

}  // namespace

std::vector<TextureQualityAction> TextureQualityGovernor::demotion_candidates(std::uint64_t epoch) const {
    std::vector<const TextureQualityObject*> objects;
    objects.reserve(textures_.size());
    for (const auto& [id, object] : textures_) {
        (void)id;
        if (object.safety == TextureQualitySafety::MipSafe &&
            object.current_level < object.demotion_steps.size() &&
            change_age_safe(object, epoch)) {
            objects.push_back(&object);
        }
    }
    std::ranges::sort(objects, [](const auto* left, const auto* right) {
        if (left->resource != right->resource) return left->resource < right->resource;
        return left->id < right->id;
    });

    std::vector<TextureQualityAction> result;
    for (const auto* object : objects) {
        for (std::uint32_t level = object->current_level; level < object->demotion_steps.size(); ++level) {
            const auto& step = object->demotion_steps[level];
            result.push_back(TextureQualityAction{
                TextureQualityAction::Type::Demote,
                object->id,
                object->resource,
                level,
                level + 1,
                step.bytes_freed,
                -(step.quality_loss * object->importance),
                demotion_candidate_score(*object, step),
            });
        }
    }
    return result;
}

std::vector<TextureQualityAction> TextureQualityGovernor::promotion_candidates(std::uint64_t epoch) const {
    std::vector<const TextureQualityObject*> objects;
    objects.reserve(textures_.size());
    for (const auto& [id, object] : textures_) {
        (void)id;
        if (object.safety == TextureQualitySafety::MipSafe && object.current_level > 0 && change_age_safe(object, epoch)) {
            objects.push_back(&object);
        }
    }
    std::ranges::sort(objects, [](const auto* left, const auto* right) {
        if (left->resource != right->resource) return left->resource < right->resource;
        return left->id < right->id;
    });

    std::vector<TextureQualityAction> result;
    for (const auto* object : objects) {
        for (std::uint32_t level = object->current_level; level > 0; --level) {
            const auto& step = object->demotion_steps[level - 1];
            result.push_back(TextureQualityAction{
                TextureQualityAction::Type::Promote,
                object->id,
                object->resource,
                level,
                level - 1,
                step.bytes_freed,
                step.quality_loss * object->importance,
                promotion_candidate_score(*object, step),
            });
        }
    }
    return result;
}

}  // namespace arc
