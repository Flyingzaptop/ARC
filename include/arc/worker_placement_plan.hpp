#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace arc {
struct WorkerCpu {std::uint32_t id{};unsigned group{},logical{},core{},efficiency{};bool allowed{true},parked{},foreign{};};
struct WorkerPlacementPlan {
    std::vector<std::uint32_t> worker_sets,game_sets;
    std::vector<unsigned> worker_logicals;
    unsigned group{},logical{},core{},physical_cores{};
    bool partition_possible{};
};
// A hardware-dependent candidate, not a claim that affinity improves performance.
// SMT siblings stay together. Prefer an efficient core, then a stable high index.
inline WorkerPlacementPlan plan_worker_placement(std::span<const WorkerCpu> cpus){
    WorkerPlacementPlan out;std::map<std::pair<unsigned,unsigned>,std::vector<WorkerCpu>> cores;
    for(const auto& cpu:cpus)cores[{cpu.group,cpu.core}].push_back(cpu);
    const std::vector<WorkerCpu>* best=nullptr;
    for(const auto& [key,siblings]:cores){
        (void)key;
        if(std::any_of(siblings.begin(),siblings.end(),[](const auto& c){return !c.allowed||c.foreign||c.parked;}))continue;
        ++out.physical_cores;
        if(!best||siblings.front().efficiency<best->front().efficiency||
           (siblings.front().efficiency==best->front().efficiency&&key>std::pair{best->front().group,best->front().core}))best=&siblings;
    }
    if(!best||out.physical_cores<2)return out;
    out.group=best->front().group;out.core=best->front().core;out.logical=best->front().logical;
    for(const auto& c:*best){out.worker_sets.push_back(c.id);out.worker_logicals.push_back(c.logical);out.logical=std::min(out.logical,c.logical);}
    for(const auto& c:cpus)if(c.allowed&&!c.foreign&&(c.group!=out.group||c.core!=out.core))out.game_sets.push_back(c.id);
    // Do not automatically take one of only two/four physical cores away.
    out.partition_possible=out.physical_cores>=6&&!out.game_sets.empty();return out;
}
}
