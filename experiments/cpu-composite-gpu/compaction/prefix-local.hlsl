// Exclusive prefix within each 256-lane group.
ByteAddressBuffer Scratch : register(t0);
RWByteAddressBuffer LocalPrefix : register(u0);
RWByteAddressBuffer GroupSums : register(u1);
RWByteAddressBuffer GroupInvalids : register(u2);
cbuffer Parameters : register(b0) { uint Count; uint ScratchWords; uint RecordWords; };
groupshared uint scan[256];
groupshared uint invalidScan[256];
[numthreads(256,1,1)]
void main(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID, uint3 group : SV_GroupID) {
  uint valid=0u, deltaLo=0u, deltaHi=0u;
  if (id.x<Count) { uint row=id.x*ScratchWords;
    valid=Scratch.Load(row*4);
    deltaLo=Scratch.Load((row+1u+RecordWords)*4);
    deltaHi=Scratch.Load((row+2u+RecordWords)*4); }
  uint emit=(valid==1u && deltaLo==RecordWords*4u && deltaHi==0u) ? 1u : 0u;
  uint invalid=(id.x<Count && (valid!=1u || deltaHi!=0u ||
    (deltaLo!=0u && deltaLo!=RecordWords*4u))) ? 1u : 0u;
  scan[tid.x]=emit; invalidScan[tid.x]=invalid; GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint offset=1u; offset<256u; offset<<=1u) {
    uint add = tid.x >= offset ? scan[tid.x-offset] : 0u;
    uint bad = tid.x >= offset ? invalidScan[tid.x-offset] : 0u;
    GroupMemoryBarrierWithGroupSync();
    scan[tid.x] += add; invalidScan[tid.x] += bad; GroupMemoryBarrierWithGroupSync();
  }
  if (id.x < Count) LocalPrefix.Store(id.x*4, scan[tid.x]-emit);
  if (tid.x == 255u) { GroupSums.Store(group.x*4, scan[255]);
    GroupInvalids.Store(group.x*4, invalidScan[255]); }
}
