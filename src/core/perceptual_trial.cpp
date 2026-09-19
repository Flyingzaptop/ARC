#include "arc/perceptual_trial.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace arc {
namespace {
bool finite_nonnegative(double value){return std::isfinite(value)&&value>=0;}
bool probability(double value){return finite_nonnegative(value)&&value<=1;}
bool valid_image(const ProbeCapture& image,const PerceptualGuardConfig& c){
    if(!image.state_key||!image.generation||!image.readback_complete||!image.linear_rgb||!image.width||!image.height)return false;
    const auto pixels=std::uint64_t(image.width)*image.height;
    if(pixels>c.max_pixels||pixels>SIZE_MAX/3||image.rgb.size()!=pixels*3)return false;
    return std::all_of(image.rgb.begin(),image.rgb.end(),[](float x){return std::isfinite(x)&&x>=0&&x<=1;});
}
std::optional<double> median(const std::vector<double>& values,const PerceptualGuardConfig& c){
    if(values.size()<c.minimum_timing_samples||values.size()>c.max_timing_samples)return {};
    if(!std::all_of(values.begin(),values.end(),[](double x){return std::isfinite(x)&&x>0;}))return {};
    auto copy=values;std::sort(copy.begin(),copy.end());const auto n=copy.size();
    return n%2?copy[n/2]:copy[n/2-1]*.5+copy[n/2]*.5;
}
}
PerceptualCritic::PerceptualCritic(PerceptualGuardConfig config):config_(config){
    if(!config.max_pixels||config.max_pixels>SIZE_MAX/3||!config.minimum_timing_samples||
       config.minimum_timing_samples>config.max_timing_samples||
       !probability(config.reference_mean_error)||!probability(config.reference_peak_error)||
       !probability(config.modified_mean_error)||!probability(config.modified_peak_error)||
       !probability(config.modified_tile_error)||!probability(config.maximum_baseline_drift)||
       !finite_nonnegative(config.minimum_gain_ms)||!probability(config.minimum_gain_fraction)||
       !probability(config.maximum_importance)||!probability(config.minimum_confidence))
        throw std::invalid_argument("Invalid perceptual guard configuration");
}
PerceptualVerdict PerceptualCritic::evaluate(const ProbeCapture& a,const ProbeCapture& b,const ProbeCapture& c) const {
    PerceptualVerdict v;
    if(!valid_image(a,config_)||!valid_image(b,config_)||!valid_image(c,config_)||
       a.width!=b.width||a.width!=c.width||a.height!=b.height||a.height!=c.height||
       a.state_key!=b.state_key||a.state_key!=c.state_key||a.generation!=b.generation||a.generation!=c.generation)return v;
    // Worst of the two full-quality references; reference drift is never subtracted
    // from measured damage. Fixed local tiles protect small concentrated changes.
    constexpr std::uint32_t tile=8;
    for(std::size_t y0=0;y0<a.height;y0+=tile)for(std::size_t x0=0;x0<a.width;x0+=tile){
        double tile_error=0;std::size_t count=0;
        for(auto y=y0;y<std::min(std::size_t(a.height),y0+tile);++y)for(auto x=x0;x<std::min(std::size_t(a.width),x0+tile);++x){
            const auto base=(std::size_t(y)*a.width+x)*3;
            for(std::size_t channel=0;channel<3;++channel){
                const auto i=base+channel;
                const double drift=std::abs(double(a.rgb[i])-c.rgb[i]);
                const double damage=std::max(std::abs(double(a.rgb[i])-b.rgb[i]),std::abs(double(c.rgb[i])-b.rgb[i]));
                v.reference_mean+=drift;v.reference_peak=std::max(v.reference_peak,drift);
                v.modified_mean+=damage;v.modified_peak=std::max(v.modified_peak,damage);
                tile_error+=damage;++count;
            }
        }
        v.modified_tile=std::max(v.modified_tile,tile_error/double(count));
    }
    v.reference_mean/=double(a.rgb.size());v.modified_mean/=double(a.rgb.size());
    if(v.reference_mean>config_.reference_mean_error||v.reference_peak>config_.reference_peak_error){v.reason=CriticReason::ReferenceDrift;return v;}
    if(v.modified_mean>config_.modified_mean_error||v.modified_peak>config_.modified_peak_error||v.modified_tile>config_.modified_tile_error){v.reason=CriticReason::ImageDamage;return v;}
    const auto before=median(a.gpu_ms,config_),modified=median(b.gpu_ms,config_),after=median(c.gpu_ms,config_);
    if(!before||!modified||!after){v.reason=CriticReason::InvalidTiming;return v;}
    v.before_ms=*before;v.modified_ms=*modified;v.after_ms=*after;
    const double baseline=std::min(*before,*after);
    if(std::abs(*before-*after)/baseline>config_.maximum_baseline_drift){v.reason=CriticReason::TimingDrift;return v;}
    v.gain_ms=baseline-*modified;
    if(v.gain_ms<=0||v.gain_ms<config_.minimum_gain_ms||v.gain_ms/baseline<config_.minimum_gain_fraction){v.reason=CriticReason::NoBenefit;return v;}
    v.reason=CriticReason::Accepted;return v;
}
PerceptualTrialController::PerceptualTrialController(PerceptualGuardConfig c):critic_(c){}
bool PerceptualTrialController::admitted(const PerceptualCandidate& c) const noexcept {
    const auto& k=c.capability;const auto& cfg=critic_.config();
    return k.action&&k.target&&k.generation&&k.supported&&k.reversible&&k.synchronized&&k.reference_capture&&
        c.importance.id&&probability(c.importance.score)&&c.importance.score<=cfg.maximum_importance&&
        probability(c.importance.confidence)&&c.importance.confidence>=cfg.minimum_confidence&&
        std::isfinite(c.measured_cost_ms)&&c.measured_cost_ms>0&&std::isfinite(c.expected_gain_ms)&&c.expected_gain_ms>0&&
        c.expected_gain_ms<=c.measured_cost_ms;
}
std::optional<std::size_t> PerceptualTrialController::choose(std::span<const PerceptualCandidate> candidates) const {
    if(active_||faulted_||busy_)return {};
    std::optional<std::size_t> best;double score=-1;
    for(std::size_t i=0;i<candidates.size();++i)if(admitted(candidates[i])){
        const double utility=candidates[i].expected_gain_ms/(0.01+candidates[i].importance.score);
        if(utility>score){score=utility;best=i;}
    }
    return best;
}
PerceptualTrialResult PerceptualTrialController::trial(PerceptualProbeHost& host,const PerceptualCandidate& candidate){
    PerceptualTrialResult r;
    if(faulted_){r.status=TrialStatus::Faulted;return r;}
    if(active_||busy_){r.status=TrialStatus::Busy;return r;}
    if(!admitted(candidate))return r;
    busy_=true;
    bool prepared=false;
    try {
        prepared=host.prepare(candidate.capability);
        if(!prepared){r.status=TrialStatus::ProbeUnavailable;host.finish();busy_=false;return r;}
        active_=candidate.capability;owner_=&host;
        const auto a=host.capture(ProbePhase::ReferenceBefore);
        const bool usable=a&&a->generation==active_->generation&&valid_image(*a,critic_.config())&&median(a->gpu_ms,critic_.config()).has_value();
        const bool applied=usable&&host.apply(*active_);
        const auto b=applied?host.capture(ProbePhase::Modified):std::nullopt;
        r.reference_restored=host.restore(*active_);
        if(!r.reference_restored){faulted_=true;r.status=TrialStatus::RollbackFailed;}
        else {
            const auto c=host.capture(ProbePhase::ReferenceAfter);
            if(a&&b&&c&&a->generation==active_->generation)r.verdict=critic_.evaluate(*a,*b,*c);
            if(r.verdict.accepted()&&host.apply(*active_)){
                r.status=TrialStatus::Retained;r.reference_restored=false;
            }else{
                // apply may partially change host state even on failure.
                r.reference_restored=host.restore(*active_);
                if(!r.reference_restored){faulted_=true;r.status=TrialStatus::RollbackFailed;}
                else {r.status=TrialStatus::Rejected;active_.reset();owner_=nullptr;}
            }
        }
    } catch(...) {
        r.reference_restored=prepared&&active_&&host.restore(*active_);
        if(prepared&&!r.reference_restored){faulted_=true;r.status=TrialStatus::RollbackFailed;}
        else {r.status=TrialStatus::ProbeUnavailable;active_.reset();owner_=nullptr;}
    }
    host.finish();busy_=false;return r;
}
bool PerceptualTrialController::restore(PerceptualProbeHost& host) noexcept {
    if(busy_||owner_!=&host)return !active_&&!faulted_;
    if(!active_)return !faulted_;
    if(!host.restore(*active_)){faulted_=true;return false;}
    active_.reset();owner_=nullptr;faulted_=false;return true;
}
bool PerceptualTrialController::recover(PerceptualProbeHost& host) noexcept {return restore(host);}
} // namespace arc
