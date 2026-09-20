#include "arc/optimizer_session.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace arc {
namespace {
bool positive(double x){return std::isfinite(x)&&x>0;}
bool nonnegative(double x){return std::isfinite(x)&&x>=0;}
}
OptimizerSession::OptimizerSession(OptimizerSessionConfig config):config_(config){
    if(!positive(config.target_fps)||config.target_fps>1000||!config.warmup_samples||!config.max_evidence_samples||
        !nonnegative(config.min_ssim)||config.min_ssim>1||!nonnegative(config.max_mean_error)||
        !nonnegative(config.max_tile_p99)||!nonnegative(config.min_gain_ms)||
        !nonnegative(config.min_gain_fraction)||!nonnegative(config.max_cpu_overhead_ms)||
        !nonnegative(config.max_gpu_overhead_ms))throw std::invalid_argument("Invalid optimizer session configuration");
    state_.target_frame_ms=1000/config_.target_fps;
}
void OptimizerSession::target(double fps){
    if(!positive(fps)||fps>1000)throw std::invalid_argument("Target FPS must be finite and in (0,1000]");
    config_.target_fps=fps;state_.target_frame_ms=1000/fps;
    if(state_.phase==SessionPhase::Limited&&!state_.active_action)state_.phase=SessionPhase::Discover;
}
void OptimizerSession::candidates(std::vector<SessionAction> actions){
    if(actions.size()>128)throw std::invalid_argument("Candidate capacity");
    std::vector<std::uint64_t> ids;
    for(const auto& a:actions){if(!a.id||!a.generation||!nonnegative(a.predicted_gain_ms)||!nonnegative(a.quality_cost)||!nonnegative(a.measured_cost_ms)||
        std::find(ids.begin(),ids.end(),a.id)!=ids.end())throw std::invalid_argument("Invalid or duplicate action");ids.push_back(a.id);}
    std::stable_sort(actions.begin(),actions.end(),[](const auto& a,const auto& b){return (a.gain_known?a.predicted_gain_ms:a.measured_cost_ms)/(.01+a.quality_cost)>(b.gain_known?b.predicted_gain_ms:b.measured_cost_ms)/(.01+b.quality_cost);});
    // A rejection belongs to a concrete generation, not the numeric action ID
    // forever. Newly compiled/replaced pipelines must get their own trial.
    std::erase_if(tried_,[&](auto id){
        const auto old=std::find_if(actions_.begin(),actions_.end(),[&](const auto& a){return a.id==id;});
        const auto next=std::find_if(actions.begin(),actions.end(),[&](const auto& a){return a.id==id;});
        return old==actions_.end()||next==actions.end()||!next->ready||old->generation!=next->generation;
    });
    actions_=std::move(actions);
    auto current=[&](std::uint64_t id,std::uint64_t generation){return std::any_of(actions_.begin(),actions_.end(),[&](const auto& action){return action.id==id&&action.generation==generation&&action.ready;});};
    if((pending_&&!current(pending_->id,pending_->generation))||
       (state_.active_action&&!current(state_.active_action,active_generation_)))candidate_epoch_changed_=true;
}
SessionRequest OptimizerSession::frame(double frame_ms,bool stable){
    if(state_.phase==SessionPhase::Faulted)return {};
    if(candidate_epoch_changed_||(state_.active_action&&++evidence_age_>=config_.max_evidence_samples))return scene_changed();
    if(!positive(frame_ms)||frame_ms>10000)return scene_changed();
    state_.filtered_frame_ms=samples_?state_.filtered_frame_ms*.8+frame_ms*.2:frame_ms;++samples_;
    if(!stable)return scene_changed();
    if(restore_pending_||apply_pending_||pending_)return {};
    if(samples_<config_.warmup_samples)return {};
    if(settle_){--settle_;return {};}
    if(hold_){--hold_;return {};}
    if(state_.active_action){
        state_.phase=SessionPhase::Active;
        if(!config_.maximize_fps&&state_.filtered_frame_ms+retained_gain_ms_<state_.target_frame_ms*.85){restore_pending_=true;state_.phase=SessionPhase::Recover;++state_.restores;return {SessionRequestKind::Restore,state_.active_action};}
        if(!config_.maximize_fps&&state_.filtered_frame_ms<=state_.target_frame_ms)return {};
    }
    if(!config_.maximize_fps&&state_.filtered_frame_ms<=state_.target_frame_ms){state_.phase=SessionPhase::Active;return {};}
    state_.phase=SessionPhase::Discover;
    for(const auto& action:actions_)if(action.ready&&action.before_profile&&std::find(tried_.begin(),tried_.end(),action.id)==tried_.end()){
        pending_=action;tried_.push_back(action.id);state_.phase=SessionPhase::Probe;++state_.probes;return {SessionRequestKind::Probe,action.id};
    }
    if(!profile_requested_){profile_requested_=true;return {SessionRequestKind::Profile,0};}
    for(const auto& action:actions_)if(action.ready&&std::find(tried_.begin(),tried_.end(),action.id)==tried_.end()){
        pending_=action;tried_.push_back(action.id);state_.phase=SessionPhase::Probe;++state_.probes;return {SessionRequestKind::Probe,action.id};
    }
    state_.phase=SessionPhase::Limited;return {};
}
SessionRequest OptimizerSession::evidence(const OptimizerTrialEvidence& e){
    if(candidate_epoch_changed_)return scene_changed();
    if(!pending_||state_.phase!=SessionPhase::Probe||e.action!=pending_->id||e.generation!=pending_->generation)return {};
    const auto id=pending_->id,generation=pending_->generation;const bool exact=pending_->exact_native_state&&e.exact_native_state;pending_.reset();
    if(!e.restoration_confirmed){state_.phase=SessionPhase::Faulted;++state_.rejected;return {};}
    const bool finite=positive(e.baseline_frame_ms)&&positive(e.candidate_frame_ms)&&nonnegative(e.baseline_noise_ms)&&
        (exact||(nonnegative(e.ssim)&&e.ssim<=1&&nonnegative(e.mean_error)&&nonnegative(e.tile_p99)))&&
        (!config_.enforce_component_budgets||(nonnegative(e.cpu_overhead_ms)&&nonnegative(e.gpu_overhead_ms)));
    const double gain=e.baseline_frame_ms-e.candidate_frame_ms;
    const bool quality=exact||(e.matched_reference&&e.ssim>=config_.min_ssim&&e.mean_error<=config_.max_mean_error&&e.tile_p99<=config_.max_tile_p99);
    const bool accept=e.complete&&quality&&finite&&nonnegative(e.original_frame_ms)&&(config_.maximize_fps||e.baseline_frame_ms>state_.target_frame_ms)&&
        (!config_.require_gpu_execution||exact||e.gpu_execution_confirmed)&&
        (!config_.enforce_component_budgets||(e.cpu_overhead_ms<=config_.max_cpu_overhead_ms&&e.gpu_overhead_ms<=config_.max_gpu_overhead_ms))&&
        gain>std::max({config_.min_gain_ms,e.baseline_frame_ms*config_.min_gain_fraction,e.baseline_noise_ms});
    if(!accept){++state_.rejected;state_.phase=SessionPhase::Settle;settle_=config_.settle_samples;return {};}
    if(positive(e.original_frame_ms))original_reference_ms_=e.original_frame_ms;
    else if(!state_.active_action)original_reference_ms_=e.baseline_frame_ms;
    // A replacement is a whole configuration. Adding improvements measured
    // against different incumbents invents a gain when the workload drifts.
    retained_gain_ms_=std::max(0.0,original_reference_ms_-e.candidate_frame_ms);
    apply_pending_=true;state_.active_action=id;active_generation_=generation;evidence_age_=0;return {SessionRequestKind::Apply,id};
}
void OptimizerSession::applied(std::uint64_t action,bool success){
    if(!apply_pending_||state_.active_action!=action)return;apply_pending_=false;
    if(!success){state_.phase=SessionPhase::Faulted;return;}
    ++state_.accepted;settle_=config_.settle_samples;hold_=config_.hold_samples;state_.phase=SessionPhase::Settle;
}
void OptimizerSession::restored(bool success){
    if(!restore_pending_)return;restore_pending_=false;
    if(!success){state_.phase=SessionPhase::Faulted;return;}
    state_.active_action=0;active_generation_=0;evidence_age_=0;retained_gain_ms_=original_reference_ms_=0;pending_.reset();apply_pending_=false;settle_=config_.settle_samples;hold_=0;
    state_.phase=SessionPhase::Settle;
}
SessionRequest OptimizerSession::scene_changed(){
    if(state_.phase==SessionPhase::Faulted||restore_pending_)return {};
    candidate_epoch_changed_=false;
    tried_.clear();profile_requested_=false;samples_=0;hold_=0;
    if(state_.active_action||pending_||apply_pending_){const auto action=state_.active_action?state_.active_action:pending_->id;pending_.reset();apply_pending_=false;restore_pending_=true;state_.phase=SessionPhase::Recover;++state_.restores;return {SessionRequestKind::Restore,action};}
    state_.phase=SessionPhase::Warmup;return {};
}
}
