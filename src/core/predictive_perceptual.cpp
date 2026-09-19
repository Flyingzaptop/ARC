#include "arc/predictive_perceptual.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace arc {
namespace {
bool probability(double x){return std::isfinite(x)&&x>=0&&x<=1;}
std::uint64_t add_saturated(std::uint64_t a,std::uint64_t b){return a>UINT64_MAX-b?UINT64_MAX:a+b;}
}
PredictivePerceptualOptimizer::PredictivePerceptualOptimizer(std::uint64_t context,PredictivePerceptualConfig p,PerceptualGuardConfig guards)
    :trials_(guards),config_(p),guards_(guards),context_(context){
    if(!context||!p.max_records||p.max_records>65536||!p.probe_cooldown_frames||!p.rejection_cooldown_frames||
       !probability(p.restore_predicted_coverage)||!probability(p.minimum_visibility_confidence))throw std::invalid_argument("Invalid prediction configuration");
}
const TemporalVisibilityState* PredictivePerceptualOptimizer::observation(std::span<const TemporalVisibilityState> states,VisualTrackId id,std::uint64_t frame)const noexcept {
    const TemporalVisibilityState* found=nullptr;
    for(const auto& s:states)if(s.id==id){if(found||s.frame!=frame)return nullptr;found=&s;}
    return found;
}
bool PredictivePerceptualOptimizer::uncertain(const TemporalVisibilityState* s)const noexcept {
    return !s||s->frames_since_observed||!probability(s->confidence)||s->confidence<config_.minimum_visibility_confidence||
        !s->direct_visibility_sample||!s->present_reachable_known||s->phase==VisibilityPhase::Unknown||
        !probability(s->predicted_2f)||!probability(s->predicted_8f)||!probability(s->estimated_visible_coverage);
}
bool PredictivePerceptualOptimizer::rising(const TemporalVisibilityState* s)const noexcept {
    return s&&std::max({s->estimated_visible_coverage,s->predicted_2f,s->predicted_8f})>=config_.restore_predicted_coverage;
}
PredictiveResult PredictivePerceptualOptimizer::step(PerceptualProbeHost& host,std::uint64_t frame,
    std::span<const PerceptualCandidate> candidates,std::span<const TemporalVisibilityState> states){
    PredictiveResult result;
    if(trials_.faulted()){result.decision=PredictiveDecision::Faulted;return result;}
    const bool bad_frame=!frame||frame<=frame_;
    const bool oversized=candidates.size()>config_.max_records||states.size()>65536;
    if(!bad_frame)frame_=frame;
    if(active_candidate_){
        const auto& original=*active_candidate_;
        const auto* visibility=observation(states,original.importance.id,frame);
        const PerceptualCandidate* current=nullptr;
        for(const auto& c:candidates)if(c.capability.action==original.capability.action&&c.capability.target==original.capability.target&&c.capability.generation==original.capability.generation){if(current){current=nullptr;break;}current=&c;}
        const bool missing=bad_frame||oversized||uncertain(visibility)||!current||!current->capability.supported||!current->capability.synchronized||
            !current->capability.reversible||!current->capability.reference_capture||current->importance.id!=original.importance.id||
            !probability(current->importance.confidence)||current->importance.confidence<guards_.minimum_confidence||!probability(current->importance.score);
        const bool important=rising(visibility)||(current&&current->importance.score>guards_.maximum_importance);
        if(missing||important){
            if(!trials_.restore(host)){result.decision=PredictiveDecision::Faulted;return result;}
            active_candidate_.reset();result.decision=missing?PredictiveDecision::RestoredForUncertainty:PredictiveDecision::RestoredForVisibility;
        }else result.decision=PredictiveDecision::Held;
        return result;
    }
    if(bad_frame||oversized)return result;
    std::vector<PerceptualCandidate> eligible;
    for(const auto& c:candidates){
        const auto* s=observation(states,c.importance.id,frame);
        if(uncertain(s)||rising(s))continue;
        auto copy=c;const auto& cap=c.capability;const Key key{cap.action,cap.target,cap.generation};
        const auto it=records_.find(key);
        if(it!=records_.end()){
            if(it->second.learned.quarantined||frame<it->second.eligible_frame)continue;
            if(it->second.learned.accepted)copy.expected_gain_ms=std::min(copy.expected_gain_ms,it->second.learned.accepted_gain_ewma);
        }else if(records_.size()>=config_.max_records)continue;
        eligible.push_back(copy);
    }
    const auto selected=trials_.choose(eligible);if(!selected)return result;
    const auto& c=eligible[*selected];const auto& cap=c.capability;
    auto& record=records_[Key{cap.action,cap.target,cap.generation}];
    record.learned.action=cap.action;record.learned.target=cap.target;record.learned.generation=cap.generation;
    result.trial=trials_.trial(host,c);result.decision=PredictiveDecision::Probed;
    if(result.trial.status==TrialStatus::Retained){
        auto& learned=record.learned;const double gain=result.trial.verdict.gain_ms;
        learned.accepted_gain_ewma=learned.accepted?.75*learned.accepted_gain_ewma+.25*gain:gain;
        learned.accepted=add_saturated(learned.accepted,1);record.eligible_frame=add_saturated(frame,config_.probe_cooldown_frames);active_candidate_=c;
    }else{
        record.learned.rejected=add_saturated(record.learned.rejected,1);record.eligible_frame=add_saturated(frame,config_.rejection_cooldown_frames);
        if(result.trial.verdict.reason==CriticReason::ImageDamage||trials_.faulted())record.learned.quarantined=true;
        if(trials_.faulted())result.decision=PredictiveDecision::Faulted;
    }
    return result;
}
bool PredictivePerceptualOptimizer::reset(PerceptualProbeHost& host)noexcept {
    if(!trials_.restore(host))return false;
    records_.clear();active_candidate_.reset();frame_=0;return true;
}
PerceptualLearningCheckpoint PredictivePerceptualOptimizer::checkpoint()const {
    PerceptualLearningCheckpoint state;state.context=context_;
    for(const auto& [key,record]:records_){(void)key;auto copy=record.learned;copy.cooldown_remaining=record.eligible_frame>frame_?record.eligible_frame-frame_:0;state.records.push_back(copy);}
    return state;
}
bool PredictivePerceptualOptimizer::load(const PerceptualLearningCheckpoint& state){
    if(frame_||active_candidate_||trials_.faulted()||!records_.empty()||state.version!=1||state.context!=context_||state.records.size()>config_.max_records)return false;
    std::map<Key,Record> restored;
    for(const auto& r:state.records){
        if(!r.action||!r.target||!r.generation||!std::isfinite(r.accepted_gain_ewma)||r.accepted_gain_ewma<0||
           (r.accepted==0&&r.accepted_gain_ewma!=0)||(r.accepted&&r.accepted_gain_ewma<=0)||
           r.cooldown_remaining>std::max(config_.probe_cooldown_frames,config_.rejection_cooldown_frames))return false;
        if(!restored.emplace(Key{r.action,r.target,r.generation},Record{r,r.cooldown_remaining}).second)return false;
    }
    records_=std::move(restored);return true;
}
} // namespace arc
