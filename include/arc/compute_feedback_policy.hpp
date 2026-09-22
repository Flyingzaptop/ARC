#pragma once
#include "arc/optimizer_policy.hpp"
#include <span>
namespace arc {
struct ComputeFeedbackTarget {
    std::uint64_t pipeline{};double cost_ms{};
    bool density{},samples{},mips{},comparison{},zero{};
};
struct ComputeAllocation {
    PolicyBundle policy;
    double estimated_saved_ms{},estimated_capacity_ms{},requested_saving_ms{};
    unsigned steps{};
};
// Cost priors, not measured shader-specific speedups. The fixed write/control
// component prevents treating a 4x4 shader as automatically 16 times faster.
inline double compute_cost_prior(const ComputePolicy& p) noexcept {
    return (.15+.85/(p.x_rate*p.y_rate))*(.2+.8*p.sample_percent/100.)*
        std::pow(.97,p.mip_steps)*(p.comparison_taps?.8:1.)*(p.zero_factor?.95:1.);
}
inline bool advance_compute_knob(ComputePolicy& p,const ComputeFeedbackTarget& t,unsigned knob,bool aggressive){
    if(knob==0&&t.density){
        if(p.x_rate==1&&p.y_rate==1){p.y_rate=2;return true;}
        if(p.x_rate==1&&p.y_rate==2){p.x_rate=2;return true;}
        if(aggressive&&p.x_rate==2&&p.y_rate==2){p.y_rate=4;return true;}
        if(aggressive&&p.x_rate==2&&p.y_rate==4){p.x_rate=4;return true;}
    }
    if(knob==1&&t.samples&&p.sample_percent>25){p.sample_percent-=25;return true;}
    if(knob==2&&t.mips&&p.mip_steps<(aggressive?8u:4u)){++p.mip_steps;return true;}
    if(knob==3&&t.comparison&&!p.comparison_taps){p.comparison_taps=9;return true;}
    if(knob==4&&t.zero&&!p.zero_factor){p.zero_factor=1;return true;}
    return false;
}
inline ComputeAllocation allocate_compute_budget(std::span<const ComputeFeedbackTarget> input,double intensity,bool aggressive){
    ComputeAllocation out;
    if(!std::isfinite(intensity)||intensity<=0)return out;
    intensity=std::clamp(intensity,0.,1.);
    std::vector<ComputeFeedbackTarget> targets;
    for(const auto& t:input)if(t.pipeline&&std::isfinite(t.cost_ms)&&t.cost_ms>0&&std::none_of(targets.begin(),targets.end(),[&](const auto& x){return x.pipeline==t.pipeline;}))targets.push_back(t);
    std::stable_sort(targets.begin(),targets.end(),[](const auto& a,const auto& b){return a.cost_ms>b.cost_ms;});
    if(targets.size()>PolicyBundle::capacity)targets.resize(PolicyBundle::capacity);
    std::vector<ComputePolicy> recipes;
    for(const auto& t:targets){ComputePolicy p;p.pipeline=t.pipeline;recipes.push_back(p);for(unsigned k=0;k<5;++k)while(advance_compute_knob(p,t,k,aggressive)){}out.estimated_capacity_ms+=t.cost_ms*(1-compute_cost_prior(p));}
    out.requested_saving_ms=intensity*out.estimated_capacity_ms;
    // Greedy marginal benefit: recompute after every mutation. An already cheap
    // pass cannot consume every step just because it was initially expensive.
    for(unsigned iteration=0;iteration<PolicyBundle::capacity*20&&out.estimated_saved_ms+1e-9<out.requested_saving_ms;++iteration){
        double gain=0;std::size_t best=targets.size();ComputePolicy selected;
        for(std::size_t i=0;i<targets.size();++i)for(unsigned knob=0;knob<5;++knob){auto next=recipes[i];if(!advance_compute_knob(next,targets[i],knob,aggressive))continue;
            const auto marginal=targets[i].cost_ms*(compute_cost_prior(recipes[i])-compute_cost_prior(next));
            if(marginal>gain){gain=marginal;best=i;selected=next;}
        }
        if(best==targets.size()||gain<=0)break;
        recipes[best]=selected;out.estimated_saved_ms+=gain;++out.steps;
    }
    out.policy.id=1;
    for(const auto& p:recipes)if(p.x_rate>1||p.y_rate>1||p.sample_percent<100||p.mip_steps||p.comparison_taps||p.zero_factor)out.policy.replace(p);
    if(out.policy.compute.empty())out.policy.id=0;
    return out;
}
inline PolicyBundle compute_feedback_policy(std::span<const ComputeFeedbackTarget> targets,double intensity,bool aggressive){return allocate_compute_budget(targets,intensity,aggressive).policy;}
}
