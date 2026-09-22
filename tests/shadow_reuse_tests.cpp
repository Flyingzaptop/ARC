#include "arc/shadow_reuse.hpp"
#include <array>
#include <cassert>
int main(){
    using namespace arc;
    ShadowReuse cache;std::array<std::byte,4> bytes{};
    ShadowEvidence e{1,1,1,1,0,bytes,true,true,true,true};
    assert(cache.evaluate(1,e)==ShadowDecision::RenderMissing);
    assert(cache.commit(1,e,7));assert(cache.evaluate(1,e)==ShadowDecision::RenderPending);
    e.completed_fence=7;assert(cache.evaluate(1,e)==ShadowDecision::Reuse);
    // Same address, changed contents: no stale shadow.
    bytes[1]=std::byte{1};assert(cache.evaluate(1,e)==ShadowDecision::RenderChanged);
    assert(cache.commit(1,e,8));e.completed_fence=8;
    ++e.output_write_epoch;assert(cache.evaluate(1,e)==ShadowDecision::RenderChanged);
    for(int field=0;field<7;++field){
        e={1,1,1,1,9,bytes,true,true,true,true};assert(cache.commit(1,e,9));
        if(field==0)++e.output_generation;if(field==1)++e.queue;if(field==2)++e.fence_identity;
        if(field==3)e.complete_inputs=false;if(field==4)e.whole_pass=false;
        if(field==5)e.output_preserved=false;if(field==6)e.exclusive_queue=false;
        assert(cache.evaluate(1,e)!=ShadowDecision::Reuse);
    }
    e={1,1,1,1,10,bytes,true,true,true,true};assert(cache.commit(1,e,10));
    cache.invalidate(1);assert(cache.evaluate(1,e)==ShadowDecision::RenderMissing);
    std::vector<std::byte> huge(ShadowReuse::byte_limit+1);e.inputs=huge;
    assert(!cache.commit(1,e,10));assert(cache.bytes()==0);
}
