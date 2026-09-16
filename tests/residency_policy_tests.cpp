#include "arc/residency.hpp"
#include <cstdlib>
#include <iostream>
#include <numeric>
#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while (false)
int main() {
    using namespace arc;
    {
        ResidencyGovernor governor;
        for (ResidencyId id = 1; id <= 4; ++id) {
            CHECK(governor.register_object({.id=id, .state=ResidencyState::Resident, .safety=ResidencySafety::ControlledSafe, .cost={.bytes=80}}));
        }
        CHECK(governor.register_object({.id=5, .state=ResidencyState::Resident, .safety=ResidencySafety::Pinned, .cost={.bytes=1000}}));
        CHECK(governor.register_object({.id=6, .state=ResidencyState::Resident, .safety=ResidencySafety::Unknown, .cost={.bytes=1000}}));
        governor.update_budget(1000, 900);
        CHECK(governor.pressure() == PressureState::Pressure);
        CHECK(governor.bytes_to_free() == 120);
        const auto actions = governor.plan_evictions(100);
        CHECK(actions.size() == 2);
        const auto planned = std::accumulate(actions.begin(), actions.end(), std::uint64_t{}, [](auto value, const auto& action) { return value + action.bytes; });
        CHECK(planned >= governor.bytes_to_free());
        for (const auto& action : actions) { CHECK(action.object <= 4 && action.type == ResidencyAction::Type::Evict); }
    }
    {
        ResidencyGovernor governor;
        CHECK(governor.register_object({.id=1, .state=ResidencyState::Resident, .safety=ResidencySafety::ControlledSafe, .cost={.bytes=256}}));
        CHECK(governor.note_use(1, 1, 10, 9));
        governor.update_budget(1000, 960);
        CHECK(governor.pressure() == PressureState::Emergency);
        CHECK(governor.plan_evictions(100).empty());
        CHECK(!governor.transition(1, ResidencyState::Resident, ResidencyState::Evicted));
        CHECK(governor.note_completed(1, 10));
        CHECK(!governor.plan_evictions(100).empty());
        CHECK(governor.transition(1, ResidencyState::Resident, ResidencyState::Evicted));
    }
    {
        ResidencyPolicyConfig config{}; config.prefetch_horizon_epochs = 8;
        ResidencyGovernor governor(config);
        CHECK(governor.register_object({.id=7, .state=ResidencyState::Resident, .safety=ResidencySafety::ControlledSafe, .cost={.bytes=128}}));
        CHECK(governor.note_use(7, 10, 1));
        CHECK(governor.note_use(7, 20, 2));
        const auto predicted = governor.predicted_next_use(7);
        CHECK(predicted && *predicted == 30);
        CHECK(governor.transition(7, ResidencyState::Resident, ResidencyState::Evicted));
        CHECK(governor.plan_promotions(21).empty());
        const auto promotions = governor.plan_promotions(22);
        CHECK(promotions.size() == 1 && promotions[0].object == 7 && promotions[0].predicted_use_epoch == 30);
    }
    {
        ResidencyPolicyConfig config{}; config.prefetch_horizon_epochs = 8; config.minimum_residency_age_epochs = 8;
        ResidencyGovernor governor(config);
        CHECK(governor.register_object({.id=1, .state=ResidencyState::Resident, .safety=ResidencySafety::ControlledSafe, .cost={.bytes=64}}));
        CHECK(governor.note_use(1, 70, 1));
        CHECK(governor.note_use(1, 90, 2));
        governor.update_budget(1000, 900);
        const auto early = governor.plan_evictions(100);
        CHECK(early.size() == 1 && early[0].object == 1 && early[0].predicted_use_epoch == 110);
        CHECK(governor.plan_evictions(102).empty());
    }
    {
        ResidencyGovernor governor;
        governor.update_budget(1000, 960); CHECK(governor.pressure() == PressureState::Emergency);
        for (int i=0; i<2; ++i) { governor.update_budget(1000, 700); CHECK(governor.pressure() == PressureState::Emergency); }
        governor.update_budget(1000, 700); CHECK(governor.pressure() == PressureState::Pressure);
        for (int i=0; i<2; ++i) { governor.update_budget(1000, 700); CHECK(governor.pressure() == PressureState::Pressure); }
        governor.update_budget(1000, 700); CHECK(governor.pressure() == PressureState::Normal);
    }
    return 0;
}
