#pragma once
#include "dr_api.h"
#include <cstdint>

namespace arc::cpu::dbi {
// Resolve an allocation-free QPC source in ntdll; do not use the DR UTC clock
// for durations or silently substitute zero for missing timing evidence.
struct CostClock {
    using Query = long(__stdcall*)(std::int64_t*,std::int64_t*);
    Query query{};
    std::int64_t frequency{};
    double pair_cost_ns{};
    void initialize() noexcept {
        auto* module=dr_lookup_module_by_name("ntdll.dll");
        if(!module) return;
        query=reinterpret_cast<Query>(dr_get_proc_address(module->handle,"NtQueryPerformanceCounter"));
        dr_free_module_data(module);
        std::int64_t stamp{};
        if(!query || query(&stamp,&frequency)<0 || frequency<=0) { query=nullptr; return; }
        std::uint64_t minimum=~std::uint64_t{};
        for(unsigned i=0;i<32;++i) {
            const auto a=now(),b=now();
            if(b>a && b-a<minimum) minimum=b-a;
        }
        if(minimum==~std::uint64_t{}) { query=nullptr; return; }
        pair_cost_ns=2*ns(minimum);
    }
    std::uint64_t now() const noexcept {
        std::int64_t value{};
        return query && query(&value,nullptr)>=0 && value>0 ? static_cast<std::uint64_t>(value) : 0;
    }
    double ns(std::uint64_t ticks) const noexcept {
        return frequency>0 ? double(ticks)*1e9/double(frequency) : 0;
    }
};
}
