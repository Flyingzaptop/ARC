#pragma once
#include <array>
#include <cstddef>
#include <cstring>

namespace arc {
// Only pure value-setting commands belong here. Resource barriers, bindings
// with lifetime promises, draws and synchronization are never cache entries.
class ExactStateCache {
public:
    enum Slot : unsigned { Pipeline, Viewports, Scissors, Topology, Blend, Stencil, Count };
    static constexpr std::size_t capacity=384;
    void invalidate() noexcept {for(auto& value:values_)value.known=false;}
    bool repeat(unsigned slot,const void* bytes,std::size_t count) noexcept {
        if(slot>=Count)return false;
        auto& value=values_[slot];
        if(count>capacity||(!bytes&&count)){value.known=false;return false;}
        if(value.known&&value.count==count&&(!count||std::memcmp(value.bytes.data(),bytes,count)==0))return true;
        if(count)std::memcpy(value.bytes.data(),bytes,count);
        value.count=count;value.known=true;return false;
    }
private:
    struct Value {std::array<std::byte,capacity> bytes;std::size_t count{};bool known{};};
    std::array<Value,Count> values_;
};
}
