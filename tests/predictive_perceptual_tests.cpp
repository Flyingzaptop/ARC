#include "arc/predictive_perceptual.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace arc;
void check(bool yes,const char* message){if(!yes){std::cerr<<message<<'\n';std::exit(1);}}
struct Host final:PerceptualProbeHost {
    bool changed{},damage{},restore_failure{};int applies{};
    bool prepare(const PerceptualCapability&)override{return !changed;}
    bool apply(const PerceptualCapability&)override{changed=true;++applies;return true;}
    bool restore(const PerceptualCapability&)noexcept override{if(restore_failure)return false;changed=false;return true;}
    void finish()noexcept override{}
    std::optional<ProbeCapture> capture(ProbePhase)override{
        ProbeCapture p;p.state_key=1;p.generation=1;p.width=8;p.height=8;p.readback_complete=true;p.linear_rgb=true;
        p.rgb.assign(8*8*3,changed&&damage?.9f:.4f);p.gpu_ms.assign(9,changed?2:3);return p;
    }
};
int main(){
    PerceptualCandidate c;c.capability={1,1,1,PerceptualMechanism::SrvMipRange,true,true,true,true};
    c.importance.id=10;c.importance.score=.1;c.importance.confidence=1;c.measured_cost_ms=3;c.expected_gain_ms=1;
    TemporalVisibilityState s;s.id=10;s.frame=1;s.phase=VisibilityPhase::Visible;s.confidence=1;
    s.direct_visibility_sample=true;s.present_reachable_known=true;s.estimated_visible_coverage=.005;s.predicted_2f=.005;s.predicted_8f=.005;
    PredictivePerceptualOptimizer p(123);Host host;
    auto step=[&](std::uint64_t frame){s.frame=frame;return p.step(host,frame,std::span(&c,1),std::span(&s,1));};
    check(step(1).trial.status==TrialStatus::Retained&&p.active()&&host.changed,"validated learning trial");
    check(step(2).decision==PredictiveDecision::Held,"stable low visibility retains quality");
    s.predicted_8f=.05;
    check(step(3).decision==PredictiveDecision::RestoredForVisibility&&!host.changed,"restore BEFORE current visibility rises");
    s.predicted_8f=.005;const int attempts=host.applies;
    check(step(4).decision==PredictiveDecision::Idle&&host.applies==attempts,"cooldown prevents oscillation");
    check(step(121).trial.status==TrialStatus::Retained,"new admission after cooldown");
    s.confidence=.2;
    check(step(122).decision==PredictiveDecision::RestoredForUncertainty&&!host.changed,"uncertainty restores rather than experiments");
    s.confidence=1;host.damage=true;
    check(step(241).trial.verdict.reason==CriticReason::ImageDamage&&!host.changed,"learned gains cannot override critic");
    const int rejected_attempts=host.applies;
    check(step(10000).decision==PredictiveDecision::Idle&&host.applies==rejected_attempts,"damaging action quarantined");
    auto snapshot=p.checkpoint();check(snapshot.records.size()==1&&snapshot.records[0].accepted==2&&snapshot.records[0].rejected==1&&snapshot.records[0].quarantined,"bounded causal outcomes");
    PredictivePerceptualOptimizer restored(123);check(restored.load(snapshot),"checkpoint import");
    s.frame=1;check(restored.step(host,1,std::span(&c,1),std::span(&s,1)).decision==PredictiveDecision::Idle,"quarantine survives restart");
    PredictivePerceptualOptimizer other(456);check(!other.load(snapshot)&&other.record_count()==0,"context isolation");
    auto invalid=snapshot;invalid.records.push_back(invalid.records[0]);PredictivePerceptualOptimizer duplicate(123);
    check(!duplicate.load(invalid)&&duplicate.record_count()==0,"transactional duplicate rejection");
    invalid=snapshot;invalid.records[0].accepted_gain_ewma=std::numeric_limits<double>::quiet_NaN();check(!duplicate.load(invalid),"invalid learned gain rejected");
    invalid=snapshot;invalid.version=2;check(!duplicate.load(invalid),"schema mismatch");
    check(p.reset(host)&&p.record_count()==0,"explicit reset");host.damage=false;
    check(step(1).trial.status==TrialStatus::Retained,"fresh calibrated session");
    check(step(1).decision==PredictiveDecision::RestoredForUncertainty&&!p.active(),"nonmonotonic frame fails closed");
    check(p.reset(host),"reset frame domain");s.direct_visibility_sample=false;
    check(step(1).decision==PredictiveDecision::Idle,"raster bound alone cannot authorize degradation");s.direct_visibility_sample=true;
    check(step(2).trial.status==TrialStatus::Retained,"strong observation required");
    host.restore_failure=true;s.predicted_8f=.1;
    check(step(3).decision==PredictiveDecision::Faulted&&p.faulted(),"predictive rollback failure latches fault");
    check(!p.reset(host),"reset cannot hide failed restore");host.restore_failure=false;check(p.reset(host),"actual recovery");
    PredictivePerceptualConfig limited;limited.max_records=1;PredictivePerceptualOptimizer bounded(123,limited);
    s.predicted_8f=.005;s.frame=1;host.damage=true;bounded.step(host,1,std::span(&c,1),std::span(&s,1));
    c.capability.action=2;s.frame=1000;const int before=host.applies;
    bounded.step(host,1000,std::span(&c,1),std::span(&s,1));check(bounded.record_count()==1&&host.applies==before,"record cap cannot evict quarantine to retry");
    std::cout<<"Mega G predictive restore, bounded learning, quarantine and versioned recovery PASS\n";
}
