// One-word homogeneous class: invalid is not a proven rejection.
ByteAddressBuffer Scratch:register(t0);
RWByteAddressBuffer Prefix:register(u0);
RWByteAddressBuffer GroupCounts:register(u1);
RWByteAddressBuffer GroupInvalid:register(u2);
RWByteAddressBuffer GroupFlag:register(u3);
cbuffer Parameters:register(b0){uint Count;};
groupshared uint sum[256];groupshared uint bad[256];groupshared uint flagOr[256];
[numthreads(256,1,1)]
void main(uint3 id:SV_DispatchThreadID,uint3 tid:SV_GroupThreadID,uint3 group:SV_GroupID){
 uint valid=0u,delta=0u,high=0u,flag=0u;
 if(id.x<Count){uint row=id.x*20u;valid=Scratch.Load(row);delta=Scratch.Load(row+8u);high=Scratch.Load(row+12u);flag=Scratch.Load(row+16u);}
 uint emit=(valid==1u&&delta==4u&&high==0u)?1u:0u;
 uint invalid=(id.x<Count&&emit==0u)?1u:0u;
 sum[tid.x]=emit;bad[tid.x]=invalid;flagOr[tid.x]=emit!=0u?(flag&1u):0u;
 GroupMemoryBarrierWithGroupSync();
 [unroll]for(uint step=1u;step<256u;step<<=1u){
  uint a=tid.x>=step?sum[tid.x-step]:0u;
  uint b=tid.x>=step?bad[tid.x-step]:0u;
  uint f=tid.x>=step?flagOr[tid.x-step]:0u;
  GroupMemoryBarrierWithGroupSync();
  sum[tid.x]+=a;bad[tid.x]+=b;flagOr[tid.x]|=f;
  GroupMemoryBarrierWithGroupSync();
 }
 if(id.x<Count)Prefix.Store(id.x*4u,sum[tid.x]-emit);
 if(tid.x==255u){GroupCounts.Store(group.x*4u,sum[255]);GroupInvalid.Store(group.x*4u,bad[255]);GroupFlag.Store(group.x*4u,flagOr[255]);}
}
