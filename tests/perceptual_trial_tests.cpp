#include "arc/perceptual_trial.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace arc;
void check(bool value,const char* why){if(!value){std::cerr<<why<<'\n';std::exit(1);}}
ProbeCapture image(double ms=4,float value=.5f){
    ProbeCapture p;p.state_key=123;p.generation=7;p.width=16;p.height=16;
    p.readback_complete=true;p.linear_rgb=true;p.rgb.assign(16*16*3,value);p.gpu_ms.assign(9,ms);return p;
}
PerceptualCandidate candidate(){
    PerceptualCandidate c;c.capability={1,2,7,PerceptualMechanism::SamplerMipBias,true,true,true,true};
    c.importance.id=42;c.importance.score=.1;c.importance.confidence=.95;c.measured_cost_ms=4;c.expected_gain_ms=1;return c;
}
struct Host final:PerceptualProbeHost {
    bool changed{},fail_restore{},fail_prepare{},damaged{},drift{},throw_capture{},fail_apply{},fail_reapply{},stale_reference{};
    int applies{},restores{},finishes{};
    bool prepare(const PerceptualCapability&)override{return !fail_prepare;}
    bool apply(const PerceptualCapability&)override{++applies;changed=true;return !fail_apply&&!(fail_reapply&&applies==2);}
    bool restore(const PerceptualCapability&)noexcept override{++restores;if(fail_restore)return false;changed=false;return true;}
    std::optional<ProbeCapture> capture(ProbePhase phase)override{
        if(throw_capture&&phase==ProbePhase::Modified)throw std::runtime_error("readback failed");
        auto p=image(changed?3:4,changed&&damaged?.8f:.5f);
        if(stale_reference&&phase==ProbePhase::ReferenceBefore)++p.generation;
        if(drift&&phase==ProbePhase::ReferenceAfter)++p.state_key;
        return p;
    }
    void finish()noexcept override{++finishes;}
};
int main(){
    PerceptualCritic critic;
    auto a=image(),b=image(3),c=image();
    check(critic.evaluate(a,b,c).accepted(),"independent beneficial probe");
    b.rgb[3]=.7f;check(critic.evaluate(a,b,c).reason==CriticReason::ImageDamage,"single bright feature protected");
    b=image(3);for(std::size_t y=0;y<8;++y)for(std::size_t x=0;x<8;++x)for(std::size_t k=0;k<3;++k)b.rgb[(y*16+x)*3+k]+=.012f;
    auto tile_config=PerceptualGuardConfig{};tile_config.modified_mean_error=.01;
    check(PerceptualCritic(tile_config).evaluate(a,b,c).reason==CriticReason::ImageDamage,"concentrated local damage");
    b=image(3);c.rgb[0]+=.01f;check(critic.evaluate(a,b,c).reason==CriticReason::ReferenceDrift,"animation drift cannot validate action");
    c=image();c.state_key++;check(critic.evaluate(a,b,c).reason==CriticReason::InvalidCapture,"unmatched reproducible state");
    c=image();b.generation++;check(critic.evaluate(a,b,c).reason==CriticReason::InvalidCapture,"stale resource generation");
    b=image(3);b.rgb[0]=std::numeric_limits<float>::quiet_NaN();check(critic.evaluate(a,b,c).reason==CriticReason::InvalidCapture,"NaN pixel");
    b=image(3);b.readback_complete=false;check(critic.evaluate(a,b,c).reason==CriticReason::InvalidCapture,"uncompleted fence/readback");
    b=image(3);b.linear_rgb=false;check(critic.evaluate(a,b,c).reason==CriticReason::InvalidCapture,"unsupported color encoding");
    b=image(3);c=image(5);check(critic.evaluate(a,b,c).reason==CriticReason::TimingDrift,"power or workload drift");
    c=image();b=image(4);check(critic.evaluate(a,b,c).reason==CriticReason::NoBenefit,"no fabricated speedup");
    b=image(3);b.gpu_ms.resize(1);check(critic.evaluate(a,b,c).reason==CriticReason::InvalidTiming,"sample minimum");
    b=image(3);b.gpu_ms[0]=-1;check(critic.evaluate(a,b,c).reason==CriticReason::InvalidTiming,"invalid timestamp");
    b=image(3);b.gpu_ms[0]=100;check(critic.evaluate(a,b,c).accepted(),"median resists isolated timing outlier");
    a.gpu_ms.assign(8,std::numeric_limits<double>::max());b.gpu_ms=a.gpu_ms;c.gpu_ms=a.gpu_ms;
    check(critic.evaluate(a,b,c).reason==CriticReason::NoBenefit,"finite extreme medians cannot fabricate infinite gain");
    auto cap=candidate();PerceptualTrialController ctrl;Host host;
    check(ctrl.choose(std::span(&cap,1))==0,"admission");
    auto result=ctrl.trial(host,cap);check(result.status==TrialStatus::Retained&&host.changed&&host.restores==1&&host.applies==2,"restore before retention");
    check(ctrl.trial(host,cap).status==TrialStatus::Busy,"one active action");
    Host stranger;check(!ctrl.restore(stranger)&&ctrl.active(),"wrong host cannot restore");
    check(ctrl.restore(host)&&!host.changed&&!ctrl.active(),"explicit rollback");
    host=Host{};host.damaged=true;result=ctrl.trial(host,cap);
    check(result.status==TrialStatus::Rejected&&!host.changed&&!ctrl.active(),"damage rejects despite predicted gain");
    host=Host{};host.throw_capture=true;result=ctrl.trial(host,cap);
    check(result.status==TrialStatus::ProbeUnavailable&&!host.changed&&host.finishes==1,"exception cleanup");
    host=Host{};host.fail_apply=true;result=ctrl.trial(host,cap);
    check(result.status==TrialStatus::Rejected&&!host.changed,"partially failed apply is restored");
    host=Host{};host.stale_reference=true;result=ctrl.trial(host,cap);
    check(result.status==TrialStatus::Rejected&&host.applies==0&&!host.changed,"stale reference must prevent even temporary mutation");
    host=Host{};host.fail_reapply=true;result=ctrl.trial(host,cap);
    check(result.status==TrialStatus::Rejected&&!host.changed,"failed retention is restored");
    host=Host{};host.fail_restore=true;result=ctrl.trial(host,cap);
    check(result.status==TrialStatus::RollbackFailed&&ctrl.faulted(),"failed rollback latches fault");
    const auto calls=host.applies;check(ctrl.trial(host,cap).status==TrialStatus::Faulted&&host.applies==calls,"fault prevents further actions");
    host.fail_restore=false;check(ctrl.recover(host)&&!host.changed&&!ctrl.faulted(),"recovery requires real restore");
    host=Host{};cap.importance.score=.9;check(ctrl.trial(host,cap).status==TrialStatus::NotAdmitted&&host.applies==0,"important work protected");
    cap.capability.isolated_probe=true;
    check(ctrl.trial(host,cap).status==TrialStatus::Retained,"isolated high-importance investigation still uses the critic");
    check(ctrl.restore(host),"isolated trial restore");host=Host{};host.damaged=true;
    check(ctrl.trial(host,cap).status==TrialStatus::Rejected&&!host.changed,"isolation never bypasses damage guard");
    cap=candidate();cap.capability.supported=false;check(!ctrl.choose(std::span(&cap,1)),"capability required");
    bool rejected_config=false;try{PerceptualGuardConfig cfg;cfg.minimum_timing_samples=0;PerceptualCritic bad(cfg);}catch(const std::invalid_argument&){rejected_config=true;}
    check(rejected_config,"invalid guard configuration");
    std::cout<<"Mega F critic, admission, transactional retention and fail-closed rollback PASS\n";
}
