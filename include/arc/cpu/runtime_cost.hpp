#pragma once
#include "arc/cpu/region_model.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace arc::cpu {
enum class CostAction : std::uint8_t { Original, Specialize, Memo, Incremental };
enum class CostReason : std::uint8_t { Collecting, Profitable, NoBenefit, UnknownClock };

// Samples cover the same instrumented region entry-to-exit boundary. This is
// a within-DBI action comparison, never an estimate of uninstrumented game FPS.
// Fixed storage and bounded trials; no allocator/CRT heap on the application path.
class RuntimeCost {
public:
    static constexpr unsigned actions=4, samples_per_action=9;
    struct Decision {
        CostAction action{CostAction::Original};
        CostReason reason{CostReason::Collecting};
        double original_ns{}, selected_ns{}, net_saved_ns{}, noise_ns{}, extra_cost_ns{};
    };
    struct SampleSet {
        std::array<double,samples_per_action> ns{};
        unsigned count{}, actuations{};
    };
    void reset(std::uint64_t generation, double timer_pair_ns=0, double discovery_ns=0) noexcept {
        generation_=generation; sets_={}; decision_={};
        timer_ns_=std::max(0.0,timer_pair_ns);
        discovery_ns_=std::max(0.0,discovery_ns);
        calls_since_decision_=0; invalid_=0; attempts_=0; tracking_ns_=0;
    }
    // Balanced O/S/M/I samples prevent a single startup observation becoming
    // a permanent choice. Revalidate periodically without unbounded probing.
    CostAction next() noexcept {
        if(decision_.reason==CostReason::UnknownClock) return CostAction::Original;
        if(decision_.reason!=CostReason::Collecting) {
            if(++calls_since_decision_<512) return decision_.action;
            sets_={}; decision_={}; calls_since_decision_=0; attempts_=0; tracking_ns_=0;
        }
        if(++attempts_>actions*samples_per_action*4) {
            decision_={CostAction::Original,CostReason::NoBenefit};
            return CostAction::Original;
        }
        unsigned best=0;
        for(unsigned i=1;i<actions;++i)
            if(sets_[i].count<sets_[best].count) best=i;
        return static_cast<CostAction>(best);
    }
    bool record(CostAction action, std::uint64_t generation, double elapsed_ns,
                bool actuated) noexcept {
        if(static_cast<unsigned>(action)>=actions || generation!=generation_ || !std::isfinite(elapsed_ns) || elapsed_ns<=0) {
            ++invalid_; return false;
        }
        auto& s=sets_[static_cast<unsigned>(action)];
        if(s.count>=samples_per_action) return false;
        s.ns[s.count++]=elapsed_ns;
        s.actuations+=actuated?1u:0u;
        for(const auto& set:sets_) if(set.count<samples_per_action) return true;
        evaluate(); return true;
    }
    void clock_unavailable() noexcept { decision_={CostAction::Original,CostReason::UnknownClock}; }
    const Decision& decision() const noexcept { return decision_; }
    bool collecting() const noexcept { return decision_.reason==CostReason::Collecting; }
    std::array<double,2> statistics(unsigned action) const noexcept {
        return action<actions && sets_[action].count==samples_per_action ? robust(sets_[action]) : std::array<double,2>{};
    }
    void account_tracking(double ns) noexcept {
        if(!std::isfinite(ns) || ns<0) return;
        tracking_ns_+=ns;
        if(decision_.reason==CostReason::Profitable || decision_.reason==CostReason::NoBenefit)
            decision_.extra_cost_ns+=ns/512.0;
        if(decision_.reason==CostReason::Profitable) {
            decision_.net_saved_ns-=ns/512.0;
            if(decision_.net_saved_ns<=decision_.noise_ns) {
                decision_.action=CostAction::Original; decision_.reason=CostReason::NoBenefit;
                decision_.selected_ns=decision_.original_ns; decision_.net_saved_ns=0;
            }
        }
    }
    const auto& samples() const noexcept { return sets_; }
    std::uint64_t generation() const noexcept { return generation_; }
    std::uint64_t invalid_samples() const noexcept { return invalid_; }
private:
    static std::array<double,2> robust(SampleSet sample) noexcept {
        std::sort(sample.ns.begin(),sample.ns.end());
        return {sample.ns[4],sample.ns[6]-sample.ns[2]};
    }
    void evaluate() noexcept {
        const auto original=robust(sets_[0]);
        decision_={CostAction::Original,CostReason::NoBenefit,original[0],original[0],0,original[1],
                   timer_ns_+(discovery_ns_+tracking_ns_)/512.0};
        for(unsigned i=1;i<actions;++i) {
            if(!sets_[i].actuations) continue;
            const auto candidate=robust(sets_[i]);
            // Failed guards/misses are already represented in candidate spans.
            // Do not multiply by hit rate again or hide miss/restore costs.
            ProfitEstimate estimate{1,1,original[0],candidate[0],0,
                                    timer_ns_,(discovery_ns_+tracking_ns_)/512.0};
            const double noise=std::max({original[1],candidate[1],timer_ns_*2,original[0]*0.05});
            const double saved=estimate.net_work_saved_ns();
            if(estimate.profitable() && saved>noise && saved>decision_.net_saved_ns) {
                decision_={static_cast<CostAction>(i),CostReason::Profitable,original[0],candidate[0],
                           saved,noise,timer_ns_+(discovery_ns_+tracking_ns_)/512.0};
            }
        }
        discovery_ns_=0; // charge cold admission once per generation
    }
    std::array<SampleSet,actions> sets_{};
    Decision decision_{};
    std::uint64_t generation_{}, invalid_{};
    unsigned calls_since_decision_{},attempts_{};
    double timer_ns_{},discovery_ns_{},tracking_ns_{};
};
}
