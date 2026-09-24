#include "ShaderInterop.h"
struct Params { uint width; uint height; };
PUSHCONSTANT(p,Params);
Texture2D<uint> actual_id:register(t0);
Texture2D<uint> reference_id:register(t1);
Texture2D<float> actual_depth:register(t2);
Texture2D<float> reference_depth:register(t3);
RWByteAddressBuffer result:register(u0);
[RootSignature("RootConstants(num32BitConstants=2,b999),DescriptorTable(SRV(t0,numDescriptors=4)),DescriptorTable(UAV(u0))")]
[numthreads(16,16,1)]void main(uint3 tid:SV_DispatchThreadID){
    if(tid.x>=p.width||tid.y>=p.height)return;
    int3 pixel=int3(tid.xy,0);uint ignored;
    if(actual_id.Load(pixel)!=0)result.InterlockedAdd(8,1,ignored);
    if(actual_id.Load(pixel)!=reference_id.Load(pixel))result.InterlockedAdd(0,1,ignored);
    if(asuint(actual_depth.Load(pixel))!=asuint(reference_depth.Load(pixel)))result.InterlockedAdd(4,1,ignored);
}
