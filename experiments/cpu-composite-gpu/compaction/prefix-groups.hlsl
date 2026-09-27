// At most 256 group sums for at most 65536 elements.
ByteAddressBuffer GroupSums : register(t0);
ByteAddressBuffer GroupInvalids : register(t1);
RWByteAddressBuffer GroupOffsets : register(u0);
RWByteAddressBuffer Metadata : register(u1);
cbuffer Parameters : register(b0) { uint Count; uint RecordStride; uint InitialCursorLo; uint InitialCursorHi; };
groupshared uint scan[256];
groupshared uint invalidScan[256];
[numthreads(256,1,1)]
void main(uint3 tid : SV_GroupThreadID) {
  uint groups=(Count+255u)/256u;
  uint value=tid.x<groups ? GroupSums.Load(tid.x*4) : 0u;
  scan[tid.x]=value; invalidScan[tid.x]=tid.x<groups ? GroupInvalids.Load(tid.x*4) : 0u;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint offset=1u; offset<256u; offset<<=1u) {
    uint add=tid.x>=offset ? scan[tid.x-offset] : 0u;
    uint bad=tid.x>=offset ? invalidScan[tid.x-offset] : 0u;
    GroupMemoryBarrierWithGroupSync();
    scan[tid.x]+=add; invalidScan[tid.x]+=bad; GroupMemoryBarrierWithGroupSync();
  }
  if (tid.x<groups) GroupOffsets.Store(tid.x*4,scan[tid.x]-value);
  if (tid.x==255u) {
    uint emitted=scan[255]; uint delta=emitted*RecordStride;
    uint low=InitialCursorLo+delta;
    Metadata.Store(0,emitted);
    Metadata.Store(4,low);
    Metadata.Store(8,InitialCursorHi+uint(low<InitialCursorLo));
    Metadata.Store(12,invalidScan[255]);
  }
}
