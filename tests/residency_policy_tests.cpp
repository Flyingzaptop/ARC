#include "arc/residency.hpp"

#include <iostream>
#include <numeric>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while (false)

int main() {
    using namespace arc;

    // Bounded eviction must free the minimum whole-object set and never touch pinned/unknown objects.
    {
        ResidencyGovernor governor;
        for (ResidencyId id = 1; id <= 4; ++id) {
            CHECK(governor.register_object({.id=id,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=80}}));
        }
        CHECK(governor.register_object({.id=5,.state=ResidencyState::Resident,.safety=ResidencySafety::Pinned,.cost={.bytes=1000}}));
        CHECK(governor.register_object({.id=6,.state=ResidencyState::Resident,.safety=ResidencySafety::Unknown,.cost={.bytes=1000}}));
        governor.update_budget(1000, 900);
        CHECK(governor.pressure() == PressureState::Pressure);
        CHECK(governor.bytes_to_free() == 120);
        const auto actions = governor.plan_evictions(100);
        CHECK(actions.size() == 2);
        const auto planned = std::accumulate(actions.begin(), actions.end(), std::uint64_t{}, [](auto total, const auto& action){ return total + action.bytes; });
        CHECK(planned >= 120 && planned - actions.back().bytes < 120);
        for (const auto& action : actions) CHECK(action.object <= 4);
    }

    // Fence evidence is mandatory for eviction and state transitions stay strict.
    {
        ResidencyGovernor governor;
        CHECK(governor.register_object({.id=1,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=256}}));
        CHECK(governor.note_use(1, 1, 7, 10, 9));
        governor.update_budget(1000, 960);
        CHECK(governor.plan_evictions(100).empty());
        CHECK(!governor.transition(1, ResidencyState::Resident, ResidencyState::Evicted));
        CHECK(governor.note_completed(1, 10));
        const auto actions = governor.plan_evictions(100);
        CHECK(actions.size() == 1 && actions[0].required_queue == 7 && actions[0].required_fence == 10);
        CHECK(governor.transition(1, ResidencyState::Resident, ResidencyState::Evicted));
        CHECK(!governor.transition(1, ResidencyState::Evicted, ResidencyState::Resident));
        CHECK(governor.transition(1, ResidencyState::Evicted, ResidencyState::PendingResident));
        CHECK(governor.transition(1, ResidencyState::PendingResident, ResidencyState::Resident));
    }

    // Stable periodic history produces useful prefetch predictions in Normal pressure only.
    {
        ResidencyPolicyConfig config{};
        config.prefetch_horizon_epochs = 8;
        config.promotion_ceiling = .80;
        ResidencyGovernor governor(config);
        CHECK(governor.register_object({.id=7,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=50,.reload_ms=2.0}}));
        for (std::uint64_t epoch : {10ULL,20ULL,30ULL,40ULL}) CHECK(governor.note_use(7, epoch, epoch));
        const auto prediction = governor.prediction(7, 42);
        CHECK(prediction && prediction->epoch == 50 && prediction->confidence >= config.minimum_prefetch_confidence);
        CHECK(governor.transition(7, ResidencyState::Resident, ResidencyState::Evicted));
        governor.update_budget(1000, 700);
        const auto promotions = governor.plan_promotions(42);
        CHECK(promotions.size() == 1 && promotions[0].predicted_use_epoch == 50);
        governor.update_budget(1000, 900);
        CHECK(governor.pressure() == PressureState::Pressure);
        CHECK(governor.plan_promotions(42).empty());
        CHECK(governor.require_resident(7).has_value());
    }

    // Pressure eviction uses a longer forecast guard than the actual prefetch window.
    {
        ResidencyPolicyConfig config{};
        config.prefetch_horizon_epochs = 8;
        config.eviction_prediction_guard_epochs = 32;
        config.minimum_residency_age_epochs = 8;
        ResidencyGovernor governor(config);
        CHECK(governor.register_object({.id=1,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=64}}));
        for (std::uint64_t epoch : {30ULL,50ULL,70ULL,90ULL}) CHECK(governor.note_use(1, epoch, epoch));
        governor.update_budget(1000, 900);
        const auto prediction = governor.prediction(1, 100);
        CHECK(prediction && prediction->epoch == 110 && prediction->confidence >= config.minimum_prefetch_confidence);
        CHECK(governor.plan_evictions(100).empty());
        // Confidence decays if expected uses do not happen; the object eventually becomes evictable.
        CHECK(!governor.plan_evictions(120).empty());
    }

    // Hysteresis must require multiple safe budget samples before returning to Normal.
    {
        ResidencyGovernor governor;
        governor.update_budget(1000, 960);
        CHECK(governor.pressure() == PressureState::Emergency);
        governor.update_budget(1000, 700); CHECK(governor.pressure() == PressureState::Emergency);
        governor.update_budget(1000, 700); CHECK(governor.pressure() == PressureState::Emergency);
        governor.update_budget(1000, 700); CHECK(governor.pressure() == PressureState::Pressure);
        governor.update_budget(1000, 700); CHECK(governor.pressure() == PressureState::Pressure);
        governor.update_budget(1000, 700); CHECK(governor.pressure() == PressureState::Pressure);
        governor.update_budget(1000, 700); CHECK(governor.pressure() == PressureState::Normal);
    }

    // Invalid configs fall back to the conservative defaults.
    {
        ResidencyPolicyConfig broken{};
        broken.pressure_enter = .6;
        broken.pressure_target = .9;
        ResidencyGovernor governor(broken);
        CHECK(governor.config().pressure_enter == ResidencyPolicyConfig{}.pressure_enter);
    }

    // Miss attribution uses the forecast captured at eviction time, not a hindsight prediction at reload time.
    {
        ResidencyPolicyConfig config{};
        config.post_miss_grace_epochs = 16;
        ResidencyGovernor governor(config);
        CHECK(governor.register_object({.id=1,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=100}}));
        for (std::uint64_t epoch : {10ULL,20ULL,30ULL,40ULL}) CHECK(governor.note_use(1, epoch, epoch));
        CHECK(governor.transition(1, ResidencyState::Resident, ResidencyState::Evicted));
        governor.record_eviction(1, false, 43);
        const auto snapshot = governor.find(1);
        CHECK(snapshot && snapshot->last_evicted_predicted_epoch == 50);
        CHECK(snapshot->last_evicted_prediction_confidence >= config.minimum_prefetch_confidence);
        CHECK(governor.transition(1, ResidencyState::Evicted, ResidencyState::PendingResident));
        CHECK(governor.transition(1, ResidencyState::PendingResident, ResidencyState::Resident));
        governor.record_resident(1, true, 50);
        CHECK(governor.metrics().predictable_misses == 1);
        CHECK(governor.metrics().compulsory_misses == 0);
        governor.update_budget(1000, 900);
        CHECK(governor.plan_evictions(60).empty());
        CHECK(!governor.plan_evictions(70).empty());
    }

    // A once-periodic object must lose confidence rapidly when the expected pattern disappears.
    {
        ResidencyPolicyConfig config{};
        config.minimum_residency_age_epochs = 2;
        config.prediction_stale_intervals = 3;
        ResidencyGovernor governor(config);
        CHECK(governor.register_object({.id=9,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=100}}));
        for (std::uint64_t epoch : {1ULL,2ULL,3ULL,4ULL}) CHECK(governor.note_use(9, epoch, epoch));
        const auto fresh = governor.prediction(9, 5);
        const auto stale = governor.prediction(9, 10);
        CHECK(fresh && fresh->confidence >= config.minimum_prefetch_confidence);
        CHECK(stale && stale->confidence == 0.0);
        governor.update_budget(1000, 900);
        CHECK(!governor.plan_evictions(10).empty());
    }

    // Insufficient history at eviction means the later demand miss is compulsory.
    {
        ResidencyGovernor governor;
        CHECK(governor.register_object({.id=2,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=100}}));
        CHECK(governor.note_use(2, 5, 1));
        CHECK(governor.transition(2, ResidencyState::Resident, ResidencyState::Evicted));
        governor.record_eviction(2, false, 6);
        CHECK(governor.transition(2, ResidencyState::Evicted, ResidencyState::PendingResident));
        CHECK(governor.transition(2, ResidencyState::PendingResident, ResidencyState::Resident));
        governor.record_resident(2, true, 100);
        CHECK(governor.metrics().compulsory_misses == 1);
        CHECK(governor.metrics().predictable_misses == 0);
    }

    return 0;
}
