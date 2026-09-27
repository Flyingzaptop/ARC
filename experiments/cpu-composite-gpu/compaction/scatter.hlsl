// Each emitted row writes one disjoint slot assigned by GPU prefix.
ByteAddressBuffer Scratch : register(t0);
ByteAddressBuffer LocalPrefix : register(t1);
ByteAddressBuffer GroupOffsets : register(t2);
RWByteAddressBuffer PackedRecords : register(u0);
cbuffer Parameters : register(b0) { uint Count; uint ScratchWords; uint RecordWords; };
[numthreads(256,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x>=Count) return;
  uint source=id.x*ScratchWords;
  if (Scratch.Load(source*4)!=1u ||
      Scratch.Load((source+1u+RecordWords)*4)!=RecordWords*4u ||
      Scratch.Load((source+2u+RecordWords)*4)!=0u) return;
  uint slot=GroupOffsets.Load((id.x/256u)*4)+LocalPrefix.Load(id.x*4);
  [loop] for (uint j=0u;j<RecordWords;++j)
    PackedRecords.Store((slot*RecordWords+j)*4,Scratch.Load((source+1u+j)*4));
}
