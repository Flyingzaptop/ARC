#include "arc/optimizer_session.hpp"
#include "arc/optimizer_policy.hpp"
#include "arc/exact_state_cache.hpp"
#include <cassert>
#include <limits>
#include <iostream>

int main(){
    arc::ExactStateCache states;unsigned stencil=3;
    assert(!states.repeat(arc::ExactStateCache::Stencil,&stencil,sizeof(stencil)));
    assert(states.repeat(arc::ExactStateCache::Stencil,&stencil,sizeof(stencil)));
    ++stencil;assert(!states.repeat(arc::ExactStateCache::Stencil,&stencil,sizeof(stencil)));
    states.invalidate();assert(!states.repeat(arc::ExactStateCache::Stencil,&stencil,sizeof(stencil)));
    assert(!states.repeat(arc::ExactStateCache::Stencil,nullptr,sizeof(stencil)));
    assert(!states.repeat(arc::ExactStateCache::Stencil,&stencil,sizeof(stencil)));
    arc::PolicyBundle bundle{1,{{11,1,2},{12,2,2}}};assert(bundle.valid());
    auto replacement=bundle;replacement.id=2;assert(replacement.replace({11,2,2}));
    assert(replacement.compute.size()==2&&replacement.find(12)->x_rate==2&&bundle.find(11)->x_rate==1);
    auto duplicate=bundle;duplicate.compute.push_back(duplicate.compute.front());assert(!duplicate.valid());
    auto malformed=bundle;malformed.compute.front().edge_threshold=std::numeric_limits<float>::quiet_NaN();assert(!malformed.valid());
    arc::OptimizerSessionConfig measured_config;measured_config.target_fps=120;measured_config.warmup_samples=1;
    arc::OptimizerSession measured(measured_config);
    measured.candidates({{1,1,0,1,true,1,false},{2,1,0,1,true,4,false}});
    assert(measured.frame(16).kind==arc::SessionRequestKind::Profile);
    assert(measured.frame(16).action==2); // measured expense, no fabricated gain
    measured.target(60);
    arc::OptimizerTrialEvidence no_longer_needed{2,1,true,true,true,16,12,.05,.99,.001,.01,.1,.1};
    assert(measured.evidence(no_longer_needed).kind==arc::SessionRequestKind::None);
    assert(measured.snapshot().accepted==0); // target changed while a trial ran
    arc::OptimizerSession exact_cpu(measured_config);
    exact_cpu.candidates({{21,1,0,0,true,0,false,true,true}});
    assert(exact_cpu.frame(16).action==21); // exact CPU trial can precede GPU profiling
    arc::OptimizerTrialEvidence exact_evidence{21,1,false,true,true,16,12,.05,0,0,0,.1,0};exact_evidence.exact_native_state=true;
    assert(exact_cpu.evidence(exact_evidence).kind==arc::SessionRequestKind::Apply);
    arc::OptimizerSession image_required(measured_config);image_required.candidates({{21,1,1,.1,true}});
    image_required.frame(16);image_required.frame(16);
    assert(image_required.evidence(exact_evidence).kind==arc::SessionRequestKind::None); // cannot waive image checks on GPU actions
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
    // Reaching the target does not exempt a retained setting from revalidation.
    config.max_evidence_samples=3;
    arc::OptimizerSession expiry(config);expiry.candidates({{7,1,4,.1,true}});
    expiry.frame(16);expiry.frame(16);assert(expiry.frame(16).action==7);
    evidence={7,1,true,true,true,16,8,.05,.99,.001,.01,.1,.1};
    assert(expiry.evidence(evidence).kind==arc::SessionRequestKind::Apply);expiry.applied(7,true);
    assert(expiry.frame(8).kind==arc::SessionRequestKind::None);
    assert(expiry.frame(8).kind==arc::SessionRequestKind::None);
    assert(expiry.frame(8).kind==arc::SessionRequestKind::Restore);
    expiry.restored(true);assert(expiry.snapshot().active_action==0);
    // Reloaded pipelines cannot inherit an old trial merely by reusing its ID.
    arc::OptimizerSession changed(config);changed.candidates({{8,1,4,.1,true}});
    changed.frame(16);changed.frame(16);assert(changed.frame(16).action==8);
    changed.candidates({{8,2,4,.1,true}});evidence.action=8;evidence.generation=1;
    assert(changed.evidence(evidence).kind==arc::SessionRequestKind::Restore);
    assert(changed.snapshot().accepted==0);
    changed.restored(true);
    arc::OptimizerSession retry_generation(config);retry_generation.candidates({{9,1,4,.1,true},{10,1,8,.01,false}});
    retry_generation.frame(16);retry_generation.frame(16);assert(retry_generation.frame(16).action==9);
    evidence={9,1,true,true,true,16,12,.05,.9,.001,.01,.1,.1};retry_generation.evidence(evidence);
    retry_generation.frame(16);retry_generation.frame(16);assert(retry_generation.snapshot().phase==arc::SessionPhase::Limited);
    retry_generation.candidates({{9,1,4,.1,true},{10,1,8,.01,false}});
    assert(retry_generation.frame(16).kind==arc::SessionRequestKind::None); // unchanged rejected candidate stays rejected
    retry_generation.candidates({{9,2,4,.1,true},{10,2,8,.01,false}});
    assert(retry_generation.frame(16).action==9); // new generation can retry; unsupported candidate never chosen
    std::cout<<"Target control, independent quality, generation, cost budgets, recovery and fault latch passed\n";
    arc::OptimizerSessionConfig maxconfig;maxconfig.maximize_fps=true;maxconfig.warmup_samples=1;maxconfig.hold_samples=0;maxconfig.settle_samples=0;
    arc::OptimizerSession maximize(maxconfig);maximize.candidates({{99,1,1,.1,true}});
    assert(maximize.frame(4).kind==arc::SessionRequestKind::Profile);
    assert(maximize.frame(4).kind==arc::SessionRequestKind::Probe);
    arc::OptimizerTrialEvidence fast{99,1,true,true,true,4,3,.01,.99,.001,.01,.1,.1};
    assert(maximize.evidence(fast).kind==arc::SessionRequestKind::Apply);maximize.applied(99,true);
    assert(maximize.frame(3).kind!=arc::SessionRequestKind::Restore);
    arc::OptimizerSession bad(maxconfig);bad.candidates({{99,1,1,.1,true}});bad.frame(4);bad.frame(4);fast.ssim=.5;
    assert(bad.evidence(fast).kind==arc::SessionRequestKind::None&&bad.snapshot().accepted==0);
    // Net-speed policy may accept unknown component costs, but never invents
    // them as zero or substitutes CPU submission for GPU execution evidence.
    maxconfig.enforce_component_budgets=false;maxconfig.require_gpu_execution=true;
    arc::OptimizerSession execution(maxconfig);execution.candidates({{99,1,1,.1,true}});execution.frame(4);execution.frame(4);
    fast.ssim=.99;fast.cpu_overhead_ms=fast.gpu_overhead_ms=std::numeric_limits<double>::quiet_NaN();fast.gpu_execution_confirmed=false;
    assert(execution.evidence(fast).kind==arc::SessionRequestKind::None);
    execution.scene_changed();execution.frame(4);execution.frame(4);fast.gpu_execution_confirmed=true;
    assert(execution.evidence(fast).kind==arc::SessionRequestKind::Apply);
    execution.applied(99,true);
    assert(execution.frame(3,true,maxconfig.max_evidence_samples).kind==arc::SessionRequestKind::Restore);
}
