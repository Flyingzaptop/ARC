#include "arc/residency.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " << #condition << " at line " << __LINE__ << '\n'; return 1; } } while(false)
int main(){
    using namespace arc;
    ResidencyPolicyConfig cfg{};
    cfg.minimum_residency_age_epochs=2;
    cfg.prefetch_horizon_epochs=6;
    cfg.pressure_target=.78;
    cfg.emergency_target=.72;
    cfg.promotion_ceiling=.84;
    cfg.recovery_samples=2;
    ResidencyGovernor governor(cfg);
    constexpr std::uint64_t objectBytes=100;
    constexpr std::uint64_t budget=1000;
    constexpr unsigned objects=10;
    std::array<bool,objects+1> resident{};
    for(unsigned i=1;i<=objects;++i){
        resident[i]=true;
        CHECK(governor.register_object({.id=i,.state=ResidencyState::Resident,.safety=ResidencySafety::ControlledSafe,.cost={.bytes=objectBytes,.reload_ms=.25}}));
    }
    std::uint64_t residentBytes=objects*objectBytes;
    std::uint64_t lateTotal=0, lateAfterWarmup=0, speculative=0, evictions=0, peak=residentBytes, residentSum=0;
    constexpr std::uint64_t epochs=5000, warmup=500;
    auto requested=[](std::uint64_t epoch)->unsigned {
        if(epoch%200==150) return 7 + static_cast<unsigned>((epoch/200)%3);
        if(epoch%50==25) return 6;
        if(epoch%20==10) return 5;
        return 1 + static_cast<unsigned>(epoch%4);
    };
    auto promote=[&](ResidencyId id,bool late)->bool{
        if(resident[id]) return true;
        if(!governor.require_resident(id).has_value()) return false;
        if(!governor.transition(id,ResidencyState::Evicted,ResidencyState::PendingResident)) return false;
        if(!governor.transition(id,ResidencyState::PendingResident,ResidencyState::Resident)) return false;
        resident[id]=true;
        residentBytes+=objectBytes;
        governor.record_resident(id,late);
        return true;
    };
    for(std::uint64_t epoch=1;epoch<=epochs;++epoch){
        governor.update_budget(budget,residentBytes);
        for(const auto& action:governor.plan_evictions(epoch)){
            CHECK(action.type==ResidencyAction::Type::Evict);
            CHECK(resident[action.object]);
            CHECK(governor.transition(action.object,ResidencyState::Resident,ResidencyState::Evicted));
            resident[action.object]=false;
            residentBytes-=action.bytes;
            ++evictions;
            governor.record_eviction(action.object,false);
        }
        governor.update_budget(budget,residentBytes);
        for(const auto& action:governor.plan_promotions(epoch)){
            if(!resident[action.object]){
                CHECK(promote(action.object,false));
                ++speculative;
            }
        }
        const auto id=requested(epoch);
        if(!resident[id]){
            ++lateTotal;
            if(epoch>warmup) ++lateAfterWarmup;
            CHECK(promote(id,true));
        }
        CHECK(governor.note_use(id,epoch,1,epoch,epoch));
        peak=(std::max)(peak,residentBytes);
        if(epoch>warmup) residentSum+=residentBytes;
    }
    const auto measuredEpochs=epochs-warmup;
    const auto average=static_cast<double>(residentSum)/static_cast<double>(measuredEpochs);
    std::cout << "late_total="<<lateTotal<<" late_after_warmup="<<lateAfterWarmup
              <<" speculative="<<speculative<<" evictions="<<evictions
              <<" avg_resident="<<average<<" peak="<<peak<<"\n";
    CHECK(evictions>0);
    CHECK(speculative>0);
    CHECK(lateAfterWarmup<40);
    CHECK(average<900.0);
    CHECK(peak<=1000);
    return 0;
}
