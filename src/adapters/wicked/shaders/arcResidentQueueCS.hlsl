#include "globals.hlsli"
struct Params { float3 eye; uint count; uint offset; uint padded; uint j; uint k; };
PUSHCONSTANT(p,Params);
StructuredBuffer<ShaderMeshInstance> instances:register(t0);
ByteAddressBuffer inputs:register(t1);
RWStructuredBuffer<uint4> entries:register(u0);
RWByteAddressBuffer pointers:register(u1);
#undef WICKED_ENGINE_DEFAULT_ROOTSIGNATURE
// Match the pinned DirectXPackedVector.inl software conversion, including its
// finite-overflow policy; do not substitute legacy f32tof16 rounding.
uint cpu_half(float value) {
    uint bits=asuint(value);uint sign=(bits&0x80000000u)>>16;bits&=0x7fffffffu;
    uint result;
    if(bits>0x477fe000u)result=((bits&0x7f800000u)==0x7f800000u&&(bits&0x7fffffu)!=0)?0x7fffu:0x7c00u;
    else if(bits==0)result=0;
    else {
        if(bits<0x38800000u){uint shift=113u-(bits>>23);bits=shift>=32?0:((0x800000u|(bits&0x7fffffu))>>shift);}
        else bits+=0xc8000000u;
        result=((bits+0x0fffu+((bits>>13)&1u))>>13)&0x7fffu;
    }
    return result|sign;
}
// Compare an f32 radicand with the exact square of the midpoint of adjacent
// positive normal f32 values. Their midpoint has <=25 significand bits, so
// its square fits in uint64; no floating-point approximation is used here.
int square_midpoint_compare(uint low,uint high,uint radicand) {
    uint le=(low>>23)&255u, he=(high>>23)&255u;
    uint64_t lm=(low&0x7fffffu)|0x800000u;
    uint64_t hm=(high&0x7fffffu)|0x800000u;
    uint64_t midpoint=lm+(hm<<(he-le));
    uint64_t square=midpoint*midpoint;
    uint se=(radicand>>23)&255u;
    uint64_t sm=(radicand&0x7fffffu)|(se?0x800000u:0u);
    uint effectiveExponent=se?se:1u;
    int shift=int(effectiveExponent)-2*int(le)+152;
    uint64_t scaled=sm<<uint(shift);
    return scaled<square?-1:(scaled>square?1:0);
}
float cpu_sqrt(float value) {
    if(value==0)return 0;
    uint input=asuint(value),r=asuint(sqrt(value));
    [unroll] for(uint step=0;step<4;step++) {
        int lower=square_midpoint_compare(r-1,r,input);
        int upper=square_midpoint_compare(r,r+1,input);
        if(lower<0 || (lower==0 && (r&1u)))r--;
        else if(upper>0 || (upper==0 && (r&1u)))r++;
        else break;
    }
    return asfloat(r);
}
[RootSignature("RootConstants(num32BitConstants=8,b999),DescriptorTable(SRV(t0,numDescriptors=2)),DescriptorTable(UAV(u0,numDescriptors=2))")]
[numthreads(64,1,1)] void main(uint3 tid:SV_DispatchThreadID)
{
    uint i=tid.x;if(i>=p.padded)return;
    if(p.j==0 && p.k==0) {
        uint4 result=uint4(0xffffffff,0xffffffff,0xffffffff,0);
        if(i<p.count){
            uint2 input=inputs.Load2(p.offset+i*8);
            precise float3 delta=p.eye-instances[input.x].center;
            precise float x=delta.x*delta.x,y=delta.y*delta.y,z=delta.z*delta.z;
            precise float xy=x+y;
            precise float sum=xy+z;
            float distance=cpu_sqrt(sum);
            result=uint4(input.y&0xffffff,cpu_half(distance),input.x,asuint(distance));
        }
        entries[i]=result;return;
    }
    if(p.j==0){if(i<p.count)pointers.Store(i*4,entries[i].z&0xffffff);return;}
    uint other=i^p.j;if(other<=i)return;
    uint4 a=entries[i],b=entries[other];
    bool greater=a.x>b.x || (a.x==b.x && (a.y>b.y || (a.y==b.y && a.z>b.z)));
    bool ascending=(i&p.k)==0;
    if(greater==ascending){entries[i]=b;entries[other]=a;}
}
