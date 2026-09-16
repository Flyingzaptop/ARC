#include "arc/texture_quality.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace arc {
namespace {

struct PlannedTextureState {
    const TextureQualityObject* object{};
    std::uint32_t level{};
    bool already_changed{};
};

[[nodiscard]] bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] double demotion_score(const TextureQualityObject& object, const TextureDemotionStep& step) noexcept {
    if (step.bytes_freed == 0) {
        return std::numeric_limits<double>::infinity();
    }
    return (step.quality_loss * object.importance) / static_cast<double>(step.bytes_freed);
}

[[nodiscard]] double promotion_score(const TextureQualityObject& object, const TextureDemotionStep& step) noexcept {
    if (step.bytes_freed == 0) {
        return 0.0;
    }
    return (step.quality_loss * object.importance) / static_cast<double>(step.bytes_freed);
}

}  // namespace

TextureQualityGovernor::TextureQualityGovernor(TextureQualityPolicyConfig config) : config_(config) {
    if (!std::isfinite(config_.minimum_importance) || config_.minimum_importance <= 0.0) {
        config_.minimum_importance = 0.01;
    }
}

bool TextureQualityGovernor::register_texture(TextureQualityObject object) {
    if (object.id == 0 || object.resource == 0 || object.full_resident_bytes == 0) {
        return false;
    }
    if (!std::isfinite(object.importance) || object.importance <= 0.0) {
        return false;
    }
    if (object.current_level > object.demotion_steps.size()) {
        return false;
    }

    std::uint64_t cumulative_freed = 0;
    for (const auto& step : object.demotion_steps) {
        if (step.bytes_freed == 0 || !finite_nonnegative(step.quality_loss)) {
            return false;
        }
        if (step.bytes_freed > object.full_resident_bytes - cumulative_freed) {
            return false;
        }
        cumulative_freed += step.bytes_freed;
    }

    object.importance = std::max(object.importance, config_.minimum_importance);
    return textures_.emplace(object.id, std::move(object)).second;
}

bool TextureQualityGovernor::set_importance(TextureQualityId id, double importance) {
    if (!std::isfinite(importance) || importance <= 0.0) {
        return false;
    }
    const auto it = textures_.find(id);
    if (it == textures_.end()) {
        return false;
    }
    it->second.importance = std::max(importance, config_.minimum_importance);
    return true;
}

bool TextureQualityGovernor::apply(const TextureQualityAction& action, std::uint64_t epoch) {
    const auto it = textures_.find(action.texture);
    if (it == textures_.end()) {
        return false;
    }

    auto& object = it->second;
    if (object.resource != action.resource || object.safety != TextureQualitySafety::MipSafe) {
        return false;
    }
    if (object.current_level != action.from_level) {
        return false;
    }

    switch (action.type) {
    case TextureQualityAction::Type::Demote:
        if (object.current_level >= object.demotion_steps.size() ||
            action.to_level != object.current_level + 1) {
            return false;
        }
        if (action.bytes_delta != object.demotion_steps[object.current_level].bytes_freed) {
            return false;
        }
        ++object.current_level;
        break;
    case TextureQualityAction::Type::Promote:
        if (object.current_level == 0 || action.to_level + 1 != object.current_level) {
            return false;
        }
        if (action.bytes_delta != object.demotion_steps[object.current_level - 1].bytes_freed) {
            return false;
        }
        --object.current_level;
        break;
    }

    object.last_change_epoch = epoch;
    return true;
}

std::vector<TextureQualityAction> TextureQualityGovernor::plan_demotions(
    std::uint64_t bytes_to_free,
    std::uint64_t epoch) const {
    std::vector<TextureQualityAction> actions;
    if (bytes_to_free == 0 || config_.max_demotions_per_plan == 0) {
        return actions;
    }

    std::vector<PlannedTextureState> states;
    states.reserve(textures_.size());
    for (const auto& [id, object] : textures_) {
        (void)id;
        if (object.safety != TextureQualitySafety::MipSafe ||
            object.current_level >= object.demotion_steps.size() ||
            !change_age_safe(object, epoch)) {
            continue;
        }
        states.push_back(PlannedTextureState{&object, object.current_level, false});
    }

    std::uint64_t planned = 0;
    while (planned < bytes_to_free && actions.size() < config_.max_demotions_per_plan) {
        PlannedTextureState* best = nullptr;
        double best_score = std::numeric_limits<double>::infinity();
        std::uint64_t best_bytes = 0;

        for (auto& state : states) {
            if (state.level >= state.object->demotion_steps.size()) {
                continue;
            }
            const auto& step = state.object->demotion_steps[state.level];
            const double score = demotion_score(*state.object, step);
            if (best == nullptr || score < best_score ||
                (score == best_score && step.bytes_freed > best_bytes) ||
                (score == best_score && step.bytes_freed == best_bytes && state.object->id < best->object->id)) {
                best = &state;
                best_score = score;
                best_bytes = step.bytes_freed;
            }
        }

        if (best == nullptr) {
            break;
        }

        const auto& step = best->object->demotion_steps[best->level];
        actions.push_back(TextureQualityAction{
            TextureQualityAction::Type::Demote,
            best->object->id,
            best->object->resource,
            best->level,
            best->level + 1,
            step.bytes_freed,
            -(step.quality_loss * best->object->importance),
            best_score,
        });
        planned += step.bytes_freed;
        ++best->level;
        best->already_changed = true;
    }

    return actions;
}

std::vector<TextureQualityAction> TextureQualityGovernor::plan_promotions(
    std::uint64_t headroom_bytes,
    std::uint64_t epoch) const {
    std::vector<TextureQualityAction> actions;
    if (headroom_bytes == 0 || config_.max_promotions_per_plan == 0) {
        return actions;
    }

    std::vector<PlannedTextureState> states;
    states.reserve(textures_.size());
    for (const auto& [id, object] : textures_) {
        (void)id;
        if (object.safety != TextureQualitySafety::MipSafe ||
            object.current_level == 0 ||
            !change_age_safe(object, epoch)) {
            continue;
        }
        states.push_back(PlannedTextureState{&object, object.current_level, false});
    }

    std::uint64_t planned = 0;
    while (actions.size() < config_.max_promotions_per_plan) {
        PlannedTextureState* best = nullptr;
        double best_score = -1.0;
        std::uint64_t best_bytes = 0;

        for (auto& state : states) {
            if (state.level == 0) {
                continue;
            }
            const auto& step = state.object->demotion_steps[state.level - 1];
            if (step.bytes_freed > headroom_bytes - planned) {
                continue;
            }
            const double score = promotion_score(*state.object, step);
            if (best == nullptr || score > best_score ||
                (score == best_score && step.bytes_freed < best_bytes) ||
                (score == best_score && step.bytes_freed == best_bytes && state.object->id < best->object->id)) {
                best = &state;
                best_score = score;
                best_bytes = step.bytes_freed;
            }
        }

        if (best == nullptr) {
            break;
        }

        const auto& step = best->object->demotion_steps[best->level - 1];
        actions.push_back(TextureQualityAction{
            TextureQualityAction::Type::Promote,
            best->object->id,
            best->object->resource,
            best->level,
            best->level - 1,
            step.bytes_freed,
            step.quality_loss * best->object->importance,
            best_score,
        });
        planned += step.bytes_freed;
        --best->level;
        best->already_changed = true;
    }

    return actions;
}

TextureQualityPlanSummary TextureQualityGovernor::demotion_summary(
    std::uint64_t bytes_to_free,
    std::uint64_t epoch) const {
    TextureQualityPlanSummary summary{};
    summary.requested_bytes = bytes_to_free;
    summary.actions = plan_demotions(bytes_to_free, epoch);
    for (const auto& action : summary.actions) {
        summary.planned_bytes += action.bytes_delta;
        summary.estimated_quality_delta += action.quality_delta;
    }
    summary.shortfall = summary.planned_bytes < summary.requested_bytes;
    return summary;
}

std::optional<TextureQualityObject> TextureQualityGovernor::find(TextureQualityId id) const {
    const auto it = textures_.find(id);
    if (it == textures_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::uint64_t> TextureQualityGovernor::resident_bytes(TextureQualityId id) const {
    const auto it = textures_.find(id);
    if (it == textures_.end()) {
        return std::nullopt;
    }
    return resident_bytes(it->second);
}

bool TextureQualityGovernor::change_age_safe(const TextureQualityObject& object, std::uint64_t epoch) const noexcept {
    if (epoch < object.last_change_epoch) {
        return false;
    }
    return epoch - object.last_change_epoch >= config_.minimum_change_age_epochs;
}

std::uint64_t TextureQualityGovernor::resident_bytes(const TextureQualityObject& object) const noexcept {
    std::uint64_t freed = 0;
    const auto level = std::min<std::size_t>(object.current_level, object.demotion_steps.size());
    for (std::size_t i = 0; i < level; ++i) {
        freed += object.demotion_steps[i].bytes_freed;
    }
    return object.full_resident_bytes - freed;
}

}  // namespace arc
