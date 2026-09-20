#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace arc {
enum class WorkerMode : unsigned {Normal,Prefer,Core,Partition};
struct PlacementWindow {double mean{},p95{},p99{};unsigned frames{};bool valid{true};};
struct PlacementSearchStatus {WorkerMode requested{};unsigned accepted{},rejected{},unstable{};bool done{};std::string reason{"baseline"};};
// Candidate changes are CPU placement only. A/B/A windows and two separate
// confirmations protect against scene drift, noisy tails and one lucky sample.
class WorkerPlacementSearch {
    enum class Phase {Before,Candidate,After,Hold};
    std::vector<WorkerMode> candidates_;std::size_t candidate_{};Phase phase_{};
    PlacementWindow before_,trial_,best_reference_;PlacementSearchStatus status_;
    WorkerMode best_{};double best_gain_{};unsigned confirmations_{},retries_{},held_{};
    static bool valid(const PlacementWindow& w){return w.valid&&w.frames>=120&&std::isfinite(w.mean)&&std::isfinite(w.p95)&&std::isfinite(w.p99)&&w.mean>0&&w.p95>0&&w.p99>=w.p95;}
    WorkerMode next(){confirmations_=retries_=0;if(++candidate_==candidates_.size()){phase_=Phase::Hold;status_.done=true;status_.requested=best_;status_.reason=best_==WorkerMode::Normal?"no_demonstrated_gain":"validated_placement";}else{phase_=Phase::Before;status_.requested=WorkerMode::Normal;status_.reason="next_baseline";}return status_.requested;}
public:
    explicit WorkerPlacementSearch(std::vector<WorkerMode> candidates):candidates_(std::move(candidates)){if(candidates_.empty()){phase_=Phase::Hold;status_.done=true;status_.reason="no_candidates";}}
    PlacementSearchStatus status()const{return status_;}
    bool harmful(double mean,unsigned frames)const{return frames>=16&&std::isfinite(mean)&&
        ((phase_==Phase::Candidate&&mean>before_.mean*1.25)||(phase_==Phase::Hold&&best_!=WorkerMode::Normal&&mean>best_reference_.mean*1.25));}
    WorkerMode abort(){++status_.rejected;best_=WorkerMode::Normal;best_gain_=0;
        if(phase_==Phase::Hold){candidate_=confirmations_=retries_=held_=0;phase_=Phase::Before;status_.done=false;status_.requested=WorkerMode::Normal;status_.reason="performance_regression";return WorkerMode::Normal;}return next();}
    WorkerMode observe(const PlacementWindow& window){
        if(!valid(window)){++status_.unstable;status_.reason="invalid_window";phase_=candidates_.empty()?Phase::Hold:Phase::Before;candidate_=confirmations_=retries_=held_=0;best_=WorkerMode::Normal;best_gain_=0;status_.done=candidates_.empty();status_.requested=WorkerMode::Normal;return status_.requested;}
        if(phase_==Phase::Hold){
            if(best_!=WorkerMode::Normal&&(window.mean>best_reference_.mean*1.25||window.p99>best_reference_.p99*1.25))return abort();
            // Retained placement also expires; changed workloads cannot inherit
            // a permanent affinity decision. Normal needs no recurring probes.
            if(best_!=WorkerMode::Normal&&++held_>=12){candidate_=0;confirmations_=retries_=held_=0;best_=WorkerMode::Normal;best_gain_=0;phase_=Phase::Before;status_.done=false;status_.requested=WorkerMode::Normal;status_.reason="evidence_expired";}return status_.requested;
        }
        if(candidates_.empty())return WorkerMode::Normal;
        if(phase_==Phase::Before){before_=window;phase_=Phase::Candidate;status_.requested=candidates_[candidate_];status_.reason="candidate";return status_.requested;}
        if(phase_==Phase::Candidate){trial_=window;phase_=Phase::After;status_.requested=WorkerMode::Normal;status_.reason="restore_for_reference";return status_.requested;}
        const double baseline=(before_.mean+window.mean)*.5,p95=(before_.p95+window.p95)*.5,p99=(before_.p99+window.p99)*.5;
        const bool stable=std::abs(before_.mean-window.mean)<=baseline*.03&&std::abs(before_.p95-window.p95)<=p95*.10&&std::abs(before_.p99-window.p99)<=p99*.20;
        if(!stable){++status_.unstable;confirmations_=0;status_.reason="baseline_drift";if(++retries_>=2)return next();phase_=Phase::Before;return WorkerMode::Normal;}
        const double gain=(baseline-trial_.mean)/baseline;
        const bool faster=gain>=.02&&baseline-trial_.mean>=.05;
        const bool smoother=trial_.mean<=baseline*1.005&&trial_.p99<=p99*.9&&p99-trial_.p99>=.2;
        const bool useful=(faster||smoother)&&trial_.p95<=p95*1.02&&trial_.p99<=p99*1.02;
        if(!useful){++status_.rejected;status_.reason="no_gain_or_tail_regression";return next();}
        if(++confirmations_<2){phase_=Phase::Before;status_.requested=WorkerMode::Normal;status_.reason="fresh_confirmation_baseline";return status_.requested;}
        ++status_.accepted;const auto score=std::max(gain,(p99-trial_.p99)/p99*.1);
        if(score>best_gain_){best_gain_=score;best_=candidates_[candidate_];best_reference_=trial_;}return next();
    }
};
}
