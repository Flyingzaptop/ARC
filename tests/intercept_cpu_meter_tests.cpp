#include "arc/intercept_cpu_meter.hpp"
#include <cassert>
#include <stdexcept>
#include <iostream>
#include <string_view>
struct FakeClock {
    using duration=std::chrono::nanoseconds;
    using time_point=std::chrono::time_point<FakeClock>;
    inline static duration value{};
    static time_point now(){return time_point(value);}
    static void advance(int amount){value+=duration(amount);}
};
using Meter=arc::BasicInterceptCpuMeter<FakeClock>;
int main(int argc,char** argv){
    if(argc==2&&std::string_view(argv[1])=="--benchmark"){
        using Real=arc::InterceptCpuMeter;constexpr unsigned count=500000;
        for(unsigned pass=0;pass<6;++pass){
            const bool enabled=pass%2;Real::enable(enabled);const auto before=Real::snapshot();const auto start=std::chrono::steady_clock::now();
            for(unsigned i=0;i<count;++i){Real::Scope scope(true);Real::Native native;std::atomic_signal_fence(std::memory_order_seq_cst);}
            const auto stop=std::chrono::steady_clock::now();const auto after=Real::snapshot();
            std::cout<<"{\"enabled\":"<<(enabled?"true":"false")<<",\"calls\":"<<count<<",\"wall_ns_per_call\":"<<std::chrono::duration<double,std::nano>(stop-start).count()/count<<",\"reported_own_ns_per_call\":"<<double(after.own_ns-before.own_ns)/count<<"}\n";
        }
        return 0;
    }
    Meter::enable(true);
    {
        Meter::Scope app(true);FakeClock::advance(10);
        {Meter::Native native;FakeClock::advance(100);}
        {Meter::Scope internal(false);Meter::Native must_count;FakeClock::advance(11);}
        FakeClock::advance(5);
    }
    auto value=Meter::snapshot();assert(value.own_ns==26&&value.excluded_native_ns==100&&value.calls==1);
    {
        Meter::Scope app(true);FakeClock::advance(2);
        {Meter::Scope nested(true);FakeClock::advance(8);} // counted in parent once
    }
    value=Meter::snapshot();assert(value.own_ns==36&&value.calls==2);
    {
        Meter::Scope app(true);FakeClock::advance(3);
        {Meter::Native native;FakeClock::advance(20);
            {Meter::Scope callback(true);FakeClock::advance(7);{Meter::Native driver;FakeClock::advance(13);}}
            FakeClock::advance(10);}
    }
    value=Meter::snapshot();assert(value.own_ns==46&&value.calls==4);
    try{Meter::Scope app(true);FakeClock::advance(4);Meter::Native native;FakeClock::advance(30);throw std::runtime_error("test");}catch(...){}
    {Meter::Scope app(true);FakeClock::advance(9);}
    value=Meter::snapshot();assert(value.own_ns==59&&value.calls==6);
    Meter::enable(false);{Meter::Scope off(true);Meter::Native native;FakeClock::advance(1000);}
    assert(Meter::snapshot().own_ns==59&&Meter::snapshot().calls==6);
    std::cout<<"Interceptor accounting preserves ARC native work, nested calls, callbacks, disabled mode and exception unwinding\n";
}
