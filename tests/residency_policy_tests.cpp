#include "arc/residency.hpp"
#include <cstdlib>
#include <iostream>
#include <numeric>
#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while (false)
int main() {
    using namespace arc;
    {
        ResidencyGovernor governor;
        for (ResidencyId id = 1; id <= 4; ++id) CHECK(governor.register_object({.id=id,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=80}}));
        CHECK(governor.register_object({.id=5,.state=ResidencyState::Resident,.safety=ResidencySafety::Pinned,.cost={.bytes=1000}}));
        CHECK(governor.register_object({.id=6,.state=ResidencyState::Resident,.safety=ResidencySafety::Unknown,.cost={.bytes=1000}}));
        governor.update_budget(1000,900); CHECK(governor.pressure()==PressureState::Pressure); CHECK(governor.bytes_to_free()==120);
        const auto actions=governor.plan_evictions(100); CHECK(actions.size()==2);
        const auto planned=std::accumulate(actions.begin(),actions.end(),std::uint64_t{},[](auto v,const auto& a){return v+a.bytes;});
        CHECK(planned>=governor.bytes_to_free()); CHECK(planned-actions.back().bytes<governor.bytes_to_free());
        for(const auto& a:actions) CHECK(a.object<=4&&a.type==ResidencyAction::Type::Evict);
        CHECK(governor.plan(100)==actions);
    }
    {
        ResidencyGovernor governor;
        CHECK(governor.register_object({.id=1,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=256}}));
        CHECK(governor.note_use(1,1,7,10,9)); governor.update_budget(1000,960); CHECK(governor.pressure()==PressureState::Emergency);
        CHECK(governor.plan_evictions(100).empty()); CHECK(!governor.transition(1,ResidencyState::Resident,ResidencyState::Evicted));
        CHECK(governor.note_completed(1,10)); const auto evictions=governor.plan_evictions(100);
        CHECK(evictions.size()==1&&evictions[0].required_queue==7&&evictions[0].required_fence==10);
        CHECK(governor.transition(1,ResidencyState::Resident,ResidencyState::Evicted)); CHECK(!governor.transition(1,ResidencyState::Evicted,ResidencyState::Resident));
        CHECK(governor.transition(1,ResidencyState::Evicted,ResidencyState::PendingResident)); CHECK(!governor.transition(1,ResidencyState::PendingResident,ResidencyState::Evicted));
        CHECK(governor.transition(1,ResidencyState::PendingResident,ResidencyState::Resident));
    }
    {
        ResidencyPolicyConfig config{}; config.prefetch_horizon_epochs=8; config.promotion_ceiling=.80;
        ResidencyGovernor governor(config); CHECK(governor.register_object({.id=7,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=50,.reload_ms=2.0}}));
        CHECK(governor.note_use(7,10,1)); CHECK(governor.note_use(7,20,2)); const auto predicted=governor.predicted_next_use(7,22); CHECK(predicted&&*predicted==30);
        CHECK(governor.transition(7,ResidencyState::Resident,ResidencyState::Evicted)); governor.update_budget(1000,700);
        CHECK(governor.plan_promotions(21).empty()); const auto promotions=governor.plan_promotions(22); CHECK(promotions.size()==1&&promotions[0].predicted_use_epoch==30);
        CHECK(governor.require_resident(7).has_value()); governor.update_budget(1000,900); CHECK(governor.pressure()==PressureState::Pressure);
        CHECK(governor.plan_promotions(22).empty()); CHECK(governor.require_resident(7).has_value());
    }
    {
        ResidencyPolicyConfig config{}; config.prefetch_horizon_epochs=8; config.minimum_residency_age_epochs=8;
        ResidencyGovernor governor(config); CHECK(governor.register_object({.id=1,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=64}}));
        CHECK(governor.note_use(1,70,1)); CHECK(governor.note_use(1,90,2)); governor.update_budget(1000,900);
        const auto early=governor.plan_evictions(100); CHECK(early.size()==1&&early[0].predicted_use_epoch==110); CHECK(governor.plan_evictions(102).empty());
        const auto nextCycle=governor.predicted_next_use(1,120); CHECK(nextCycle&&*nextCycle==130); CHECK(governor.plan_evictions(120).size()==1); CHECK(governor.plan_evictions(122).empty());
    }
    {
        ResidencyPolicyConfig config{}; config.promotion_ceiling=.75; ResidencyGovernor governor(config);
        CHECK(governor.register_object({.id=1,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=100}}));
        CHECK(governor.note_use(1,10,1)); CHECK(governor.note_use(1,20,2)); CHECK(governor.transition(1,ResidencyState::Resident,ResidencyState::Evicted));
        governor.update_budget(1000,730); CHECK(governor.plan_promotions(22).empty());
    }
    {
        ResidencyGovernor governor; governor.update_budget(1000,960); CHECK(governor.pressure()==PressureState::Emergency);
        for(int i=0;i<2;++i){governor.update_budget(1000,700);CHECK(governor.pressure()==PressureState::Emergency);} governor.update_budget(1000,700);CHECK(governor.pressure()==PressureState::Pressure);
        for(int i=0;i<2;++i){governor.update_budget(1000,700);CHECK(governor.pressure()==PressureState::Pressure);} governor.update_budget(1000,700);CHECK(governor.pressure()==PressureState::Normal);
    }
    {
        ResidencyPolicyConfig broken{}; broken.pressure_enter=.6; broken.pressure_target=.9; ResidencyGovernor governor(broken); CHECK(governor.config().pressure_enter==ResidencyPolicyConfig{}.pressure_enter);
    }
    return 0;
}
