#include "arc/cpu/runtime_cost.hpp"
#include <cassert>
using namespace arc::cpu;
static void feed(RuntimeCost& cost, const double (&ns)[4], bool actuated=true) {
    for(unsigned n=0;n<RuntimeCost::samples_per_action*4;++n) {
        const auto action=cost.next();
        assert(cost.record(action,7,ns[static_cast<unsigned>(action)],actuated));
    }
}
int main() {
    RuntimeCost cost; cost.reset(7,5,512);
    feed(cost,{1000,1200,500,750});
    assert(cost.decision().action==CostAction::Memo);
    assert(cost.decision().reason==CostReason::Profitable);
    assert(cost.decision().net_saved_ns==494);
    cost.account_tracking(300000);
    assert(cost.decision().reason==CostReason::NoBenefit);
    cost.reset(7,5,512); feed(cost,{100,101,102,103});
    assert(cost.decision().reason==CostReason::NoBenefit);
    assert(cost.next()==CostAction::Original);
    cost.reset(7,40,51200); feed(cost,{1000,900,880,930});
    assert(cost.decision().reason==CostReason::NoBenefit); // costs erase apparent gain
    cost.reset(7); feed(cost,{1000,400,300,200},false);
    assert(cost.decision().reason==CostReason::NoBenefit); // no executed work, no winner
    cost.reset(7);
    assert(!cost.record(CostAction::Memo,8,1,true));
    assert(!cost.record(CostAction::Memo,7,-1,true));
    assert(cost.invalid_samples()==2);
    cost.clock_unavailable(); assert(cost.next()==CostAction::Original);
}
