#include "arc/worker_placement_plan.hpp"
#include <cassert>
#include <iostream>
int main(){
    std::vector<arc::WorkerCpu> cpus;
    for(unsigned i=0;i<16;++i)cpus.push_back({256+i,0,i,i/2,1});
    auto plan=arc::plan_worker_placement(cpus);assert(plan.physical_cores==8&&plan.partition_possible);
    assert((plan.worker_sets==std::vector<std::uint32_t>{270,271})&&plan.game_sets.size()==14);
    cpus[0].efficiency=cpus[1].efficiency=0;plan=arc::plan_worker_placement(cpus);assert(plan.core==0);
    cpus[0].foreign=true;plan=arc::plan_worker_placement(cpus);assert(plan.core==7&&plan.physical_cores==7); // don't steal the other SMT sibling
    cpus[14].parked=true;plan=arc::plan_worker_placement(cpus);assert(plan.core==6);
    cpus[12].allowed=false;plan=arc::plan_worker_placement(cpus);assert(plan.core==5&&!plan.partition_possible);
    cpus.resize(8);for(auto& c:cpus){c.foreign=false;c.allowed=true;c.parked=false;}
    plan=arc::plan_worker_placement(cpus);assert(!plan.worker_sets.empty()&&!plan.partition_possible);
    cpus.resize(2);assert(arc::plan_worker_placement(cpus).worker_sets.empty());
    cpus={{1,0,0,0,1},{2,1,0,0,1}};plan=arc::plan_worker_placement(cpus);assert(plan.physical_cores==2&&plan.group==1); // group-relative core IDs
    assert(arc::plan_worker_placement({}).worker_sets.empty());
    std::cout<<"Worker placement preserves SMT topology, affinity exclusions, heterogeneous classes, small CPUs and groups\n";
}
