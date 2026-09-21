#include "../benchmarks/cauldron/arc_async_writer.h"
#include <sstream>
#include <cassert>
#include <iostream>
int main(){
    std::ostringstream output;arc_bench::AsyncWriter writer(output);
    for(unsigned i=0;i<5000;++i)writer.push([i](auto& out){out<<i<<'\n';});
    assert(writer.finish());std::istringstream input(output.str());for(unsigned i=0;i<5000;++i){unsigned n=6000;input>>n;assert(n==i);}
    std::atomic<bool> entered{},release{};std::ostringstream dropped;arc_bench::AsyncWriter bounded(dropped);
    bounded.push([&](auto&){entered=true;while(!release.load())std::this_thread::yield();});
    while(!entered.load())std::this_thread::yield();
    for(unsigned i=0;i<9000;++i)bounded.push([](auto& out){out<<'.';});
    release=true;assert(!bounded.finish());
    std::ostringstream failed;arc_bench::AsyncWriter failing(failed);failing.push([](auto&){throw 1;});assert(!failing.finish());
    std::cout<<"Ordered asynchronous telemetry, overflow and failure reporting passed\n";
}
