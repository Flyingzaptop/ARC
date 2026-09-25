// Source-assisted Wicked experiment. Sorted records are never reordered.
cbuffer Params:register(b0){uint N;uint Stencil;uint ObjectCount;uint DrawCount;}
ByteAddressBuffer records:register(t0);
ByteAddressBuffer objects:register(t1);
ByteAddressBuffer extra:register(t2); // float transparency, uint raw LOD, uint alpha-test predicate
RWStructuredBuffer<uint4> work:register(u0);
RWStructuredBuffer<uint2> prefix:register(u1);
RWStructuredBuffer<uint2> blocks:register(u2);
RWStructuredBuffer<uint> metadata:register(u3); // header 4 words; 256 groups * 8 words
RWStructuredBuffer<uint> output:register(u4);
ByteAddressBuffer templates:register(t3);
RWStructuredBuffer<uint> arguments:register(u5);
groupshared uint2 sums[256];
uint3 key(uint i){uint4 r=records.Load4(i*16);uint obj=min(r.y,ObjectCount-1);return uint3(r.x,(r.w&255)==255?(extra.Load(obj*20+4)&255):(r.w&255),Stencil?objects.Load(obj*256+8)>>24:0);}
[numthreads(256,1,1)] void evaluate(uint3 dt:SV_DispatchThreadID,uint li:SV_GroupIndex,uint3 gid:SV_GroupID){
 uint i=dt.x;uint4 w=0;uint2 v=0;
 if(i<N){uint4 r=records.Load4(i*16);uint obj=min(r.y,ObjectCount-1);
  if(r.y>=ObjectCount){InterlockedOr(metadata[2],1);}
  else{
   float distance=f16tof32(r.z&65535),fade=asfloat(objects.Load(obj*256+44)),radius=asfloat(objects.Load(obj*256+92));
   float transparency=asfloat(extra.Load(obj*20));
   // Restricted experiment rejects nonfinite/zero-radius inputs, rather than guessing CPU NaN semantics.
   if(!isfinite(distance)||!isfinite(fade)||!isfinite(radius)||radius<=0||!isfinite(transparency)){InterlockedOr(metadata[2],2);}
   else {precise float delta=distance-fade;precise float d=max(transparency,max(0.f,delta)/radius);
    uint mask=d>0.99f?0:r.z>>16;uint base=(obj&0xffffff)|((uint(max(d,0.f)*15.f)&15)<<28);
    uint start=i==0?1:(any(key(i)!=key(i==0?0:i-1))?1:0);
    w=uint4(base,mask,start,d<=0.99f && (d>0 || extra.Load(obj*20+8)!=0));v=uint2(countbits(mask),start);
   }
  }
  work[i]=w;
 }
 sums[li]=v;GroupMemoryBarrierWithGroupSync();
 for(uint s=1;s<256;s*=2){uint2 a=li>=s?sums[li-s]:0;GroupMemoryBarrierWithGroupSync();sums[li]+=a;GroupMemoryBarrierWithGroupSync();}
 if(i<N)prefix[i]=uint2(sums[li].x-v.x,sums[li].y);
 if(li==255)blocks[gid.x]=sums[li];
}
[numthreads(256,1,1)] void scanblocks(uint li:SV_GroupIndex){
 uint count=(N+255)/256;uint2 v=li<count?blocks[li]:0;sums[li]=v;GroupMemoryBarrierWithGroupSync();
 for(uint s=1;s<256;s*=2){uint2 a=li>=s?sums[li-s]:0;GroupMemoryBarrierWithGroupSync();sums[li]+=a;GroupMemoryBarrierWithGroupSync();}
 if(li<count)blocks[li]=sums[li]-v;
 if(li==255){metadata[0]=sums[li].y;metadata[1]=sums[li].x;}
}
[numthreads(256,1,1)] void groups(uint3 dt:SV_DispatchThreadID){uint i=dt.x;if(i>=N)return;
 uint2 p=prefix[i]+blocks[i/256];prefix[i]=p;
 if(work[i].z!=0 && p.y<=256){uint g=p.y-1,o=4+g*8;uint3 k=key(i);metadata[o]=k.x;metadata[o+1]=k.y;metadata[o+2]=k.z;metadata[o+3]=0;metadata[o+4]=p.x;metadata[o+5]=0;metadata[o+6]=i;metadata[o+7]=0;}
}
[numthreads(256,1,1)] void scatter(uint3 dt:SV_DispatchThreadID){uint i=dt.x;if(i>=N)return;uint4 w=work[i];uint2 p=prefix[i];
 uint bits=w.y,pos=p.x;while(bits){uint bit=firstbitlow(bits);bits^=1u<<bit;output[pos++]=w.x|(bit<<24);}
 if(w.w!=0 && p.y>0 && p.y<=256)InterlockedOr(metadata[4+(p.y-1)*8+3],1);
}
[numthreads(256,1,1)] void finish(uint li:SV_GroupIndex){uint count=metadata[0];if(li>=count||li>=256)return;uint o=4+li*8;uint end=li+1<count&&li+1<256?metadata[o+12]:metadata[1];metadata[o+5]=end-metadata[o+4];metadata[o+7]=li+1<count&&li+1<256?metadata[o+14]:N;}

[numthreads(64,1,1)] void drawargs(uint i:SV_GroupIndex){if(i>=DrawCount)return;uint o=i*5;arguments[o]=templates.Load(o*4);arguments[o+1]=metadata[2]==0 && metadata[0]==1?metadata[1]:0;arguments[o+2]=templates.Load((o+2)*4);arguments[o+3]=0;arguments[o+4]=metadata[8];}
