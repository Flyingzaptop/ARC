// Single 256-lane scan covers at most 65536 input words.
ByteAddressBuffer GroupCounts:register(t0);
ByteAddressBuffer GroupInvalid:register(t1);
ByteAddressBuffer GroupFlag:register(t2);
RWByteAddressBuffer GroupOffsets:register(u0);
RWByteAddressBuffer Metadata:register(u1);
cbuffer Parameters:register(b0){uint Count;uint InitialIndex;uint InitialCounter;uint InitialFlag;
 uint LastByte;uint LastWord;uint LastQwordLo;uint LastQwordHi;};
groupshared uint sum[256];groupshared uint bad[256];groupshared uint flagOr[256];
[numthreads(256,1,1)]
void main(uint3 tid:SV_GroupThreadID){
 uint groups=(Count+255u)/256u;uint active=tid.x<groups;
 uint value=active!=0u?GroupCounts.Load(tid.x*4u):0u;
 sum[tid.x]=value;bad[tid.x]=active!=0u?GroupInvalid.Load(tid.x*4u):0u;
 flagOr[tid.x]=active!=0u?GroupFlag.Load(tid.x*4u):0u;
 GroupMemoryBarrierWithGroupSync();
 [unroll]for(uint step=1u;step<256u;step<<=1u){
  uint a=tid.x>=step?sum[tid.x-step]:0u;
  uint b=tid.x>=step?bad[tid.x-step]:0u;
  uint f=tid.x>=step?flagOr[tid.x-step]:0u;
  GroupMemoryBarrierWithGroupSync();
  sum[tid.x]+=a;bad[tid.x]+=b;flagOr[tid.x]|=f;
  GroupMemoryBarrierWithGroupSync();
 }
 if(active!=0u)GroupOffsets.Store(tid.x*4u,sum[tid.x]-value);
 if(tid.x==255u){
  uint emitted=sum[255];Metadata.Store(0u,emitted);
  Metadata.Store(4u,InitialIndex+emitted);Metadata.Store(8u,InitialCounter+emitted);
  Metadata.Store(12u,InitialFlag|flagOr[255]);Metadata.Store(16u,bad[255]);
  Metadata.Store(20u,LastWord);Metadata.Store(24u,LastQwordLo);
  Metadata.Store(28u,LastQwordHi);Metadata.Store(32u,LastByte);
 }
}
