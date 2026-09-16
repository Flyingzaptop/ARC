#include "arc/texture_quality.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <unordered_map>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << " seed=" << seed << '\n'; return 1; } } while (false)

int main() {
    using namespace arc;

    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        std::mt19937_64 rng(seed);
        TextureQualityPolicyConfig config{};
        config.minimum_change_age_epochs = 0;
        config.max_demotions_per_plan = 128;
        config.max_promotions_per_plan = 128;
        TextureQualityGovernor governor(config);

        std::unordered_map<TextureQualityId, TextureQualitySafety> safety;
        std::unordered_map<TextureQualityId, std::uint32_t> expected_level;

        constexpr std::uint32_t kTextureCount = 96;
        for (TextureQualityId id = 1; id <= kTextureCount; ++id) {
            TextureQualityObject object{};
            object.id = id;
            object.resource = 10000 + id;
            const auto safety_roll = static_cast<unsigned>(rng() % 10);
            object.safety = safety_roll < 7 ? TextureQualitySafety::MipSafe
                : (safety_roll < 9 ? TextureQualitySafety::Unknown : TextureQualitySafety::Pinned);
            object.importance = 0.1 + static_cast<double>(rng() % 1000) / 100.0;

            const std::uint32_t step_count = 1 + static_cast<std::uint32_t>(rng() % 4);
            std::uint64_t step_sum = 0;
            for (std::uint32_t step = 0; step < step_count; ++step) {
                const std::uint64_t bytes = 1 + (rng() % 128);
                const double loss = static_cast<double>(1 + (rng() % 10000)) / 10000.0;
                object.demotion_steps.push_back({bytes, loss});
                step_sum += bytes;
            }
            object.full_resident_bytes = step_sum + 1 + (rng() % 256);
            object.current_level = static_cast<std::uint32_t>(rng() % (step_count + 1));

            CHECK(governor.register_texture(object));
            safety[id] = object.safety;
            expected_level[id] = object.current_level;
        }

        for (std::uint64_t epoch = 1; epoch <= 1000; ++epoch) {
            if ((rng() % 4) == 0) {
                const TextureQualityId id = 1 + (rng() % kTextureCount);
                const double importance = 0.01 + static_cast<double>(rng() % 5000) / 100.0;
                CHECK(governor.set_importance(id, importance));
            }

            if ((rng() & 1ULL) == 0) {
                const std::uint64_t request = rng() % 1024;
                const auto actions = governor.plan_demotions(request, epoch);
                std::uint64_t planned = 0;
                for (const auto& action : actions) {
                    CHECK(action.type == TextureQualityAction::Type::Demote);
                    CHECK(safety[action.texture] == TextureQualitySafety::MipSafe);
                    CHECK(action.from_level == expected_level[action.texture]);
                    CHECK(action.to_level == action.from_level + 1);
                    CHECK(action.bytes_delta > 0);
                    planned += action.bytes_delta;
                    CHECK(governor.apply(action, epoch));
                    expected_level[action.texture] = action.to_level;
                }
                const auto summary = governor.demotion_summary(0, epoch);
                CHECK(summary.actions.empty());
                CHECK(planned >= request || actions.size() < config.max_demotions_per_plan);
            } else {
                const std::uint64_t headroom = rng() % 1024;
                const auto actions = governor.plan_promotions(headroom, epoch);
                std::uint64_t planned = 0;
                for (const auto& action : actions) {
                    CHECK(action.type == TextureQualityAction::Type::Promote);
                    CHECK(safety[action.texture] == TextureQualitySafety::MipSafe);
                    CHECK(action.from_level == expected_level[action.texture]);
                    CHECK(action.from_level > 0);
                    CHECK(action.to_level + 1 == action.from_level);
                    CHECK(action.bytes_delta > 0);
                    CHECK(planned + action.bytes_delta <= headroom);
                    planned += action.bytes_delta;
                    CHECK(governor.apply(action, epoch));
                    expected_level[action.texture] = action.to_level;
                }
            }

            for (TextureQualityId id = 1; id <= kTextureCount; ++id) {
                const auto object = governor.find(id);
                CHECK(object.has_value());
                CHECK(object->current_level == expected_level[id]);
                CHECK(object->current_level <= object->demotion_steps.size());
                const auto bytes = governor.resident_bytes(id);
                CHECK(bytes.has_value());
                CHECK(*bytes <= object->full_resident_bytes);
                CHECK(*bytes > 0);
                if (safety[id] != TextureQualitySafety::MipSafe) {
                    CHECK(object->current_level == expected_level[id]);
                }
            }
        }
    }

    return 0;
}
