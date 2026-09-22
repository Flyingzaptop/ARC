#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace arc {
// Caller serializes access and owns resource lifetimes/generation identities.
// Exact pass memoization. This is not an object-motion heuristic. A backend
// must supply a complete canonical snapshot of all inputs, including mutable
// buffer contents, pipeline state and geometry. Unknown inputs never hit.
struct ShadowEvidence {
    std::uint64_t output_generation{}, output_write_epoch{}, queue{}, fence_identity{}, completed_fence{};
    std::span<const std::byte> inputs;
    bool complete_inputs{}, whole_pass{}, output_preserved{}, exclusive_queue{};
};
enum class ShadowDecision { RenderUnknown, RenderMissing, RenderChanged, RenderPending, Reuse };
class ShadowReuse {
    struct Entry {
        std::uint64_t generation{}, epoch{}, queue{}, fence{}, value{};
        std::vector<std::byte> inputs;
    };
    std::map<std::uint64_t,Entry> entries_;
    std::size_t bytes_{};
    static bool valid(const ShadowEvidence& e) {
        return e.output_generation&&e.queue&&e.fence_identity&&e.complete_inputs&&e.whole_pass&&
            e.output_preserved&&e.exclusive_queue&&!e.inputs.empty();
    }
public:
    static constexpr std::size_t byte_limit=1024*1024,entry_limit=64;
    void invalidate(std::uint64_t output) {
        if(auto i=entries_.find(output);i!=entries_.end()){bytes_-=i->second.inputs.size();entries_.erase(i);}
    }
    void clear(){entries_.clear();bytes_=0;}
    std::size_t bytes()const{return bytes_;}
    ShadowDecision evaluate(std::uint64_t output,const ShadowEvidence& e) {
        if(!output||!valid(e)){invalidate(output);return ShadowDecision::RenderUnknown;}
        auto i=entries_.find(output);if(i==entries_.end())return ShadowDecision::RenderMissing;
        const auto& x=i->second;
        if(x.generation!=e.output_generation||x.epoch!=e.output_write_epoch||x.queue!=e.queue||
           x.fence!=e.fence_identity||x.inputs.size()!=e.inputs.size()||
           !std::equal(x.inputs.begin(),x.inputs.end(),e.inputs.begin())) {
            invalidate(output);return ShadowDecision::RenderChanged;
        }
        if(e.completed_fence<x.value)return ShadowDecision::RenderPending;
        return ShadowDecision::Reuse;
    }
    // Call invalidate BEFORE submitting any output write. Commit is allowed
    // only after the complete original pass has been submitted and signalled.
    bool commit(std::uint64_t output,const ShadowEvidence& e,std::uint64_t signalled_fence) {
        invalidate(output);
        if(!output||!valid(e)||!signalled_fence||e.inputs.size()>byte_limit||
           entries_.size()>=entry_limit||e.inputs.size()>byte_limit-bytes_)return false;
        Entry x{e.output_generation,e.output_write_epoch,e.queue,e.fence_identity,signalled_fence,
                std::vector<std::byte>(e.inputs.begin(),e.inputs.end())};
        entries_.emplace(output,std::move(x));bytes_+=e.inputs.size();return true;
    }
};
}
