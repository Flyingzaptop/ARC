#include "arc/optimizer_session.hpp"
#include <cassert>
#include <limits>
#include <iostream>

int main(){
    arc::OptimizerSessionConfig config;config.target_fps=120;config.warmup_samples=2;config.settle_samples=1;config.hold_samples=2;
    arc::OptimizerSession session(config);session.candidates({{1,5,4,.1,true},{2,9,2,.1,true}});
    assert(session.frame(16).kind==arc::SessionRequestKind::None);
    assert(session.frame(16).kind==arc::SessionRequestKind::Profile);
    auto first=session.frame(16);assert(first.kind==arc::SessionRequestKind::Probe&&first.action==1);
    arc::OptimizerTrialEvidence evidence{1,5,true,true,true,16,12,.05,.97,.001,.01,.1,.1};
    assert(session.evidence(evidence).kind==arc::SessionRequestKind::None); // fast but visibly worse
    session.frame(16);auto second=session.frame(16);assert(second.kind==arc::SessionRequestKind::Probe&&second.action==2);
    evidence.action=2;evidence.generation=8;evidence.ssim=.99;
    assert(session.evidence(evidence).kind==arc::SessionRequestKind::None); // stale generation does not finish trial
    evidence.generation=9;evidence.matched_reference=false;
    assert(session.evidence(evidence).kind==arc::SessionRequestKind::None); // uncertain quality cannot be accepted
    session.frame(16);session.frame(16);assert(session.snapshot().phase==arc::SessionPhase::Limited);
    assert(session.snapshot().accepted==0&&session.snapshot().rejected==2);
    session.scene_changed();session.candidates({{3,1,4,.05,true},{4,1,6,.1,true}});
    session.frame(16);session.frame(16);auto retry=session.frame(16);assert(retry.action==3);
    evidence={3,1,true,true,true,16,12,.05,.99,.001,.01,.1,.1};auto apply=session.evidence(evidence);
    assert(apply.kind==arc::SessionRequestKind::Apply&&apply.action==3);session.applied(3,true);
    for(int i=0;i<3;++i)session.frame(12);
    auto stronger=session.frame(12);assert(stronger.kind==arc::SessionRequestKind::Probe&&stronger.action==4);
    evidence={4,1,true,true,true,12,8,.05,.99,.001,.01,.3,.1}; // own CPU overhead exceeds budget
    assert(session.evidence(evidence).kind==arc::SessionRequestKind::None);
    assert(session.snapshot().active_action==3);
    auto restore=session.scene_changed();assert(restore.kind==arc::SessionRequestKind::Restore&&restore.action==3);
    session.restored(true);assert(session.snapshot().active_action==0);
    session.frame(16);session.frame(16);session.frame(16);session.frame(16);
    auto state=session.snapshot();assert(state.phase==arc::SessionPhase::Probe);
    auto invalid=evidence;invalid.action=3;invalid.generation=1;invalid.restoration_confirmed=false;
    session.evidence(invalid);assert(session.snapshot().phase==arc::SessionPhase::Faulted);
    assert(session.frame(16).kind==arc::SessionRequestKind::None);
    bool rejected=false;try{session.target(std::numeric_limits<double>::quiet_NaN());}catch(...){rejected=true;}assert(rejected);
    std::cout<<"Target control, independent quality, generation, cost budgets, recovery and fault latch passed\n";
}
