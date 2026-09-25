#pragma once
#include <cstdint>
#include <cstddef>
namespace arc_indirect_contract {
struct Extra{float transparency;uint32_t lod,alpha,flags,generation;};
struct Group{uint32_t mesh,lod,stencil;};
inline bool reusable(uint64_t owner,uint64_t consumerCompleted){return owner==~0ull||consumerCompleted>=owner+1;}
inline bool admit(const uint32_t* records,uint32_t n,const Extra* side,size_t objects,uint32_t generation,Group& group){
 if(n==0||n>65536||objects==0)return false;
 for(uint32_t i=0;i<n;++i){auto* b=records+i*4;uint32_t obj=b[1];if(obj>=objects||(b[2]&0x7c00)==0x7c00)return false;const auto& e=side[obj];if(e.generation!=generation||!(e.flags&256))return false;uint32_t lod=(b[3]&255)==255?(e.lod&255):(b[3]&255),stencil=e.flags&255;
  if(i==0)group={b[0],lod,stencil};else if(b[0]!=group.mesh||lod!=group.lod||stencil!=group.stencil)return false;
 }
 return true;
}
}
