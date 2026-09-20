#include "arc/worker_placement_search.hpp"
#include <cassert>
#include <iostream>
int main(){
    using M=arc::WorkerMode;const arc::PlacementWindow base{10,11,12,120},fast{9,10,11,120};
    arc::WorkerPlacementSearch search({M::Core});assert(search.observe(base)==M::Core);assert(search.observe(fast)==M::Normal);assert(search.observe(base)==M::Core);
    assert(search.status().accepted==0);assert(search.observe(fast)==M::Normal);assert(search.observe(base)==M::Core);assert(search.status().accepted==1&&search.status().done);
    for(int i=0;i<11;++i)assert(search.observe(fast)==M::Core);assert(search.observe(fast)==M::Normal);assert(!search.status().done);
    arc::WorkerPlacementSearch tail({M::Core});tail.observe(base);tail.observe({8,10,20,120});assert(tail.observe(base)==M::Normal&&tail.status().rejected==1);
    arc::WorkerPlacementSearch noise({M::Prefer});noise.observe(base);noise.observe({9.95,11,12,120});assert(noise.observe(base)==M::Normal&&noise.status().done);
    arc::WorkerPlacementSearch drift({M::Core});drift.observe(base);drift.observe(fast);assert(drift.observe({8,9,10,120})==M::Normal);assert(drift.status().unstable==1&&drift.status().accepted==0);
    drift.observe(base);drift.observe(fast);drift.observe({8,9,10,120});assert(drift.status().done&&drift.status().accepted==0);
    arc::WorkerPlacementSearch invalid({M::Core});invalid.observe(base);assert(invalid.observe({1,1,1,0})==M::Normal&&invalid.status().accepted==0);
    arc::WorkerPlacementSearch none({});assert(none.observe(base)==M::Normal&&none.status().done);
    arc::WorkerPlacementSearch emergency({M::Core});emergency.observe(base);assert(!emergency.harmful(20,8));assert(emergency.harmful(20,16));assert(emergency.abort()==M::Normal&&emergency.status().rejected==1);
    arc::WorkerPlacementSearch invalid_hold({M::Core});invalid_hold.observe(base);invalid_hold.observe(fast);invalid_hold.observe(base);invalid_hold.observe(fast);invalid_hold.observe(base);
    assert(invalid_hold.status().done);assert(invalid_hold.observe({})==M::Normal);assert(invalid_hold.observe(base)==M::Core); // reset cursor as well as phase
    std::cout<<"Placement search requires repeated A/B/A gains, rejects noisy/tail regressions, and expires retained evidence\n";
}
