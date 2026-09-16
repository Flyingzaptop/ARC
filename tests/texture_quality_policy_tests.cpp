#include "arc/texture_quality.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while (false)

namespace {

arc::TextureQualityObject texture(
    arc::TextureQualityId id,
    arc::TextureQualitySafety safety,
    std::uint64_t full_bytes,
    std::vector<arc::TextureDemotionStep> steps,
    std::uint32_t level = 0,
    double importance = 1.0,
    std::uint64_t last_change = 0) {
    arc::TextureQualityObject object{};
    object.id = id;
    object.resource = id + 1000;
    object.safety = safety;
    object.full_resident_bytes = full_bytes;
    object.demotion_steps = std::move(steps);
    object.current_level = level;
    object.importance = importance;
    object.last_change_epoch = last_change;
    return object;
}

}  // namespace

int main() {
    using namespace arc;

    {
        TextureQualityPolicyConfig config{};
        config.minimum_change_age_epochs = 0;
        TextureQualityGovernor governor(config);
        CHECK(governor.register_texture(texture(1, TextureQualitySafety::MipSafe, 100, {{40, 0.80}})));
        CHECK(governor.register_texture(texture(2, TextureQualitySafety::MipSafe, 100, {{30, 0.03}})));
        CHECK(governor.register_texture(texture(3, TextureQualitySafety::MipSafe, 100, {{50, 0.06}})));

        const auto actions = governor.plan_demotions(70, 1);
        CHECK(actions.size() == 2);
        CHECK(actions[0].texture == 2);
        CHECK(actions[1].texture == 3);
        CHECK(actions[0].bytes_delta + actions[1].bytes_delta == 80);

        const auto summary = governor.demotion_summary(70, 1);
        CHECK(summary.planned_bytes == 80);
        CHECK(!summary.shortfall);
        CHECK(std::abs(summary.estimated_quality_delta + 0.09) < 1e-12);
    }

    {
        TextureQualityPolicyConfig config{};
        config.minimum_change_age_epochs = 0;
        TextureQualityGovernor governor(config);
        CHECK(governor.register_texture(texture(1, TextureQualitySafety::Pinned, 500, {{400, 0.001}})));
        CHECK(governor.register_texture(texture(2, TextureQualitySafety::Unknown, 500, {{400, 0.001}})));
        CHECK(governor.register_texture(texture(3, TextureQualitySafety::MipSafe, 100, {{40, 0.5}})));
        const auto actions = governor.plan_demotions(200, 1);
        CHECK(actions.size() == 1);
        CHECK(actions[0].texture == 3);
        const auto summary = governor.demotion_summary(200, 1);
        CHECK(summary.shortfall);
        CHECK(summary.planned_bytes == 40);
    }

    {
        TextureQualityPolicyConfig config{};
        config.minimum_change_age_epochs = 0;
        TextureQualityGovernor governor(config);
        CHECK(governor.register_texture(texture(1, TextureQualitySafety::MipSafe, 100, {{10, 10.0}, {90, 0.01}})));
        CHECK(governor.register_texture(texture(2, TextureQualitySafety::MipSafe, 100, {{50, 1.0}})));
        const auto actions = governor.plan_demotions(60, 1);
        CHECK(actions.size() == 2);
        CHECK(actions[0].texture == 2);
        CHECK(actions[1].texture == 1);
        CHECK(actions[1].from_level == 0);
        CHECK(actions[1].to_level == 1);
    }

    {
        TextureQualityPolicyConfig config{};
        config.minimum_change_age_epochs = 0;
        TextureQualityGovernor governor(config);
        CHECK(governor.register_texture(texture(7, TextureQualitySafety::MipSafe, 100, {{40, 0.4}, {20, 0.3}}, 2)));
        CHECK(governor.resident_bytes(7).value() == 40);

        const auto one = governor.plan_promotions(40, 1);
        CHECK(one.size() == 1);
        CHECK(one[0].from_level == 2 && one[0].to_level == 1);
        CHECK(one[0].bytes_delta == 20);

        const auto both = governor.plan_promotions(60, 1);
        CHECK(both.size() == 2);
        CHECK(both[0].bytes_delta == 20);
        CHECK(both[1].bytes_delta == 40);

        CHECK(governor.apply(both[0], 1));
        CHECK(governor.resident_bytes(7).value() == 60);
        CHECK(governor.apply(both[1], 1));
        CHECK(governor.resident_bytes(7).value() == 100);
        CHECK(!governor.apply(both[1], 1));
    }

    {
        TextureQualityPolicyConfig config{};
        config.minimum_change_age_epochs = 8;
        TextureQualityGovernor governor(config);
        CHECK(governor.register_texture(texture(1, TextureQualitySafety::MipSafe, 100, {{50, 0.1}}, 0, 1.0, 10)));
        CHECK(governor.plan_demotions(1, 17).empty());
        CHECK(governor.plan_demotions(1, 18).size() == 1);
    }

    {
        TextureQualityPolicyConfig config{};
        config.minimum_change_age_epochs = 0;
        TextureQualityGovernor governor(config);
        CHECK(governor.register_texture(texture(1, TextureQualitySafety::MipSafe, 200, {{100, 1.0}}, 0, 0.1)));
        CHECK(governor.register_texture(texture(2, TextureQualitySafety::MipSafe, 200, {{100, 0.2}})));
        CHECK(governor.plan_demotions(50, 1)[0].texture == 1);
        CHECK(governor.set_importance(1, 10.0));
        CHECK(governor.plan_demotions(50, 1)[0].texture == 2);
        CHECK(!governor.set_importance(1, 0.0));
    }

    {
        TextureQualityGovernor governor;
        CHECK(!governor.register_texture(texture(1, TextureQualitySafety::MipSafe, 100, {{80, 0.1}, {30, 0.1}})));
        CHECK(!governor.register_texture(texture(2, TextureQualitySafety::MipSafe, 100, {{0, 0.1}})));
        CHECK(!governor.register_texture(texture(3, TextureQualitySafety::MipSafe, 100, {{50, -1.0}})));
    }

    return 0;
}
