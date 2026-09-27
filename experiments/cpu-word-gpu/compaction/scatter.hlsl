// Output buffer starts at independently guarded entry index.
ByteAddressBuffer Scratch:register(t0);
ByteAddressBuffer Prefix:register(t1);
ByteAddressBuffer GroupOffsets:register(t2);
RWByteAddressBuffer PackedWords:register(u0);
cbuffer Parameters:register(b0){uint Count;};
[numthreads(256,1,1)]
void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Count)return;uint row=id.x*20u;
 if(Scratch.Load(row)!=1u||Scratch.Load(row+8u)!=4u||Scratch.Load(row+12u)!=0u)return;
 uint slot=GroupOffsets.Load((id.x/256u)*4u)+Prefix.Load(id.x*4u);
 PackedWords.Store(slot*4u,Scratch.Load(row+4u));
}
