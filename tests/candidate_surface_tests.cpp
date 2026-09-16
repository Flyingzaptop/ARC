#include "arc/residency.hpp"
#include "arc/texture_quality.hpp"

#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while(false)

int main() {
    using namespace arc;

    // Local residency planning should stop once its byte target is met, while
    // the candidate surface still exposes every locally-safe alternative.
    {
        ResidencyPolicyConfig config{};
        config.minimum_residency_age_epochs = 1;
        ResidencyGovernor governor(config);
        for (ResidencyId id = 1; id <= 4; ++id) {
            CHECK(governor.register_object({
                .id=id,.resource=100+id,.state=ResidencyState::Resident,
                .safety=ResidencySafety::ControlledSafe,.cost={.bytes=100,.reload_ms=static_cast<double>(id)}}));
            CHECK(governor.note_use(id, 1, 1));
        }
        governor.update_budget(1000, 900); // 120 bytes requested by default pressure target.
        const auto local = governor.plan_evictions(100);
        const auto all = governor.eviction_candidates(100);
        CHECK(!local.empty());
        CHECK(local.size() < all.size());
        CHECK(all.size() == 4);
    }

    // A protected near-term periodic object is absent from both local and global-safe surfaces.
    {
        ResidencyPolicyConfig config{};
        config.minimum_residency_age_epochs = 1;
        config.eviction_prediction_guard_epochs = 32;
        ResidencyGovernor governor(config);
        CHECK(governor.register_object({.id=1,.resource=1,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=100}}));
        for (std::uint64_t epoch : {10ULL,20ULL,30ULL,40ULL}) CHECK(governor.note_use(1, epoch, epoch));
        governor.update_budget(1000, 900);
        CHECK(governor.eviction_candidates(42).empty());
    }

    // Texture local plan truncates at requested bytes; candidate surface exposes
    // the complete sequential quality ladder and preserves actual mip levels.
    {
        TextureQualityPolicyConfig config{};
        config.minimum_change_age_epochs = 0;
        TextureQualityGovernor governor(config);
        CHECK(governor.register_texture({
            .id=1,.resource=11,.safety=TextureQualitySafety::MipSafe,.full_resident_bytes=180,
            .demotion_steps={{80,.01},{50,.03},{30,.08}},.importance=1.0}));
        const auto local = governor.plan_demotions(70, 1);
        const auto all = governor.demotion_candidates(1);
        CHECK(local.size() == 1);
        CHECK(all.size() == 3);
        CHECK(all[0].from_level == 0 && all[1].from_level == 1 && all[2].from_level == 2);

        CHECK(governor.apply(all[0], 1));
        const auto after = governor.demotion_candidates(2);
        CHECK(after.size() == 2);
        CHECK(after[0].from_level == 1 && after[1].from_level == 2);
    }

    // Restore surface exposes every legal reverse step from the current state.
    {
        TextureQualityPolicyConfig config{};
        config.minimum_change_age_epochs = 0;
        TextureQualityGovernor governor(config);
        CHECK(governor.register_texture({
            .id=2,.resource=22,.safety=TextureQualitySafety::MipSafe,.full_resident_bytes=180,
            .demotion_steps={{80,.01},{50,.03},{30,.08}},.current_level=3,.importance=1.0}));
        const auto promotions = governor.promotion_candidates(10);
        CHECK(promotions.size() == 3);
        CHECK(promotions[0].from_level == 3 && promotions[0].to_level == 2);
        CHECK(promotions[1].from_level == 2 && promotions[1].to_level == 1);
        CHECK(promotions[2].from_level == 1 && promotions[2].to_level == 0);
    }

    // Pinned/unknown quality objects never leak into the safe candidate surface.
    {
        TextureQualityPolicyConfig config{};
        config.minimum_change_age_epochs = 0;
        TextureQualityGovernor governor(config);
        CHECK(governor.register_texture({.id=3,.resource=33,.safety=TextureQualitySafety::Pinned,.full_resident_bytes=100,.demotion_steps={{50,.1}}}));
        CHECK(governor.register_texture({.id=4,.resource=44,.safety=TextureQualitySafety::Unknown,.full_resident_bytes=100,.demotion_steps={{50,.1}}}));
        CHECK(governor.demotion_candidates(100).empty());
    }

    return 0;
}
