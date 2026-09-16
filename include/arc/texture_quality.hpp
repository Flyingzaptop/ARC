#pragma once

#include "arc/ids.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace arc {

using TextureQualityId = std::uint64_t;

enum class TextureQualitySafety : std::uint8_t {
    Unknown,
    MipSafe,
    Pinned,
};

// One incremental transition from quality level N to N+1.
// bytes_freed is the physical memory recovered by this single transition,
// not the total size of the destination mip set.
struct TextureDemotionStep {
    std::uint64_t bytes_freed{};
    double quality_loss{};
};

struct TextureQualityObject {
    TextureQualityId id{};
    ResourceId resource{};
    TextureQualitySafety safety{TextureQualitySafety::Unknown};
    std::uint64_t full_resident_bytes{};
    std::vector<TextureDemotionStep> demotion_steps{};
    std::uint32_t current_level{};
    double importance{1.0};
    std::uint64_t last_change_epoch{};
};

struct TextureQualityPolicyConfig {
    std::uint32_t max_demotions_per_plan{64};
    std::uint32_t max_promotions_per_plan{64};
    std::uint64_t minimum_change_age_epochs{8};
    double minimum_importance{0.01};
};

struct TextureQualityAction {
    enum class Type : std::uint8_t {
        Demote,
        Promote,
    } type{};

    TextureQualityId texture{};
    ResourceId resource{};
    std::uint32_t from_level{};
    std::uint32_t to_level{};
    std::uint64_t bytes_delta{};
    double quality_delta{};
    double score{};

    friend bool operator==(const TextureQualityAction&, const TextureQualityAction&) = default;
};

struct TextureQualityPlanSummary {
    std::uint64_t requested_bytes{};
    std::uint64_t planned_bytes{};
    double estimated_quality_delta{};
    bool shortfall{};
    std::vector<TextureQualityAction> actions{};
};

class TextureQualityGovernor final {
public:
    explicit TextureQualityGovernor(TextureQualityPolicyConfig config = {});

    bool register_texture(TextureQualityObject object);
    bool set_importance(TextureQualityId id, double importance);
    bool apply(const TextureQualityAction& action, std::uint64_t epoch);

    [[nodiscard]] std::vector<TextureQualityAction> plan_demotions(
        std::uint64_t bytes_to_free,
        std::uint64_t epoch) const;

    [[nodiscard]] std::vector<TextureQualityAction> plan_promotions(
        std::uint64_t headroom_bytes,
        std::uint64_t epoch) const;

    [[nodiscard]] TextureQualityPlanSummary demotion_summary(
        std::uint64_t bytes_to_free,
        std::uint64_t epoch) const;

    [[nodiscard]] std::optional<TextureQualityObject> find(TextureQualityId id) const;
    [[nodiscard]] std::optional<std::uint64_t> resident_bytes(TextureQualityId id) const;
    [[nodiscard]] const TextureQualityPolicyConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] bool change_age_safe(const TextureQualityObject& object, std::uint64_t epoch) const noexcept;
    [[nodiscard]] std::uint64_t resident_bytes(const TextureQualityObject& object) const noexcept;

    std::unordered_map<TextureQualityId, TextureQualityObject> textures_{};
    TextureQualityPolicyConfig config_{};
};

}  // namespace arc
