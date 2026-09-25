// Isolated exact-order port. Parallel children operate only on disjoint ranges.
// A partition retains the original sequential swap order, including equal keys.
cbuffer Params : register(b0) { uint count; uint source; uint ideal; uint capacity; };
RWStructuredBuffer<uint4> data : register(u0);
RWStructuredBuffer<uint3> tasks0 : register(u1);
RWStructuredBuffer<uint3> tasks1 : register(u2);
RWStructuredBuffer<uint> counts : register(u3);
RWByteAddressBuffer args : register(u4);
uint2 key(uint4 v) { return uint2((v.z&65535)|((v.w&255)<<16)|((v.x&255)<<24), ((v.x>>8)&255)|(v.w&0xffffff00)); }
bool lessKey(uint2 a,uint2 b) { return a.y<b.y || (a.y==b.y && a.x<b.x); }
bool less(uint4 a,uint4 b) { return lessKey(key(a),key(b)); }
void exchange(uint a,uint b) { uint4 v=data[a]; data[a]=data[b]; data[b]=v; }
void median3(uint a,uint b,uint c) {
 if(less(data[b],data[a]))exchange(a,b);
 if(less(data[c],data[b])) { exchange(b,c); if(less(data[b],data[a]))exchange(a,b); }
}
uint2 partition(uint lo,uint hi) {
 uint mid=lo+(hi-lo)/2, last=hi-1;
 if(last-lo>40) { uint step=(hi-lo)/8; median3(lo,lo+step,lo+2*step); median3(mid-step,mid,mid+step); median3(last-2*step,last-step,last); median3(lo+step,mid,last-step); }
 else median3(lo,mid,last);
 uint left=mid,right=mid+1;
 uint2 pivot=key(data[mid]);
 [loop] while(lo<left) { if(any(key(data[left-1])!=pivot))break; --left; }
 [loop] while(right<hi) { if(any(key(data[right])!=pivot))break; ++right; }
 uint up=right,down=left;
 [loop] for(;;) {
  [loop] while(up<hi) {
   uint2 k=key(data[up]);
   if(lessKey(pivot,k)) {} else if(lessKey(k,pivot))break;
   else { if(right!=up)exchange(right,up); ++right; } ++up;
  }
  [loop] while(lo<down) {
   uint prev=down-1;uint2 k=key(data[prev]);
   if(lessKey(k,pivot)) {} else if(lessKey(pivot,k))break;
   else { --left;if(left!=prev)exchange(left,prev); } --down;
  }
  if(down==lo && up==hi)return uint2(left,right);
  if(down==lo) { if(right!=up)exchange(left,right);++right;exchange(left,up);++left;++up; }
  else if(up==hi) { --down;--left;if(down!=left)exchange(down,left);--right;exchange(left,right); }
  else { --down;exchange(up,down);++up; }
 }
}
void insertion(uint lo,uint hi) {
 [loop] for(uint i=lo+1;i<hi;++i) {
  uint4 value=data[i];uint hole=i;
  [loop] while(hole>lo) { uint4 prev=data[hole-1];if(!less(value,prev))break;data[hole]=prev;--hole; }
  data[hole]=value;
 }
}
void heapHole(uint lo,uint hole,uint n,uint4 value) {
 uint top=hole;
 [loop] while(hole<(n-1)/2) {
  uint child=2*hole+2;if(less(data[lo+child],data[lo+child-1]))--child;
  data[lo+hole]=data[lo+child];hole=child;
 }
 if(hole==(n-1)/2 && (n&1)==0) { data[lo+hole]=data[lo+n-1];hole=n-1; }
 [loop] while(hole>top) { uint parent=(hole-1)/2;uint4 p=data[lo+parent];if(!less(p,value))break;data[lo+hole]=p;hole=parent; }
 data[lo+hole]=value;
}
void heap(uint lo,uint hi) {
 uint n=hi-lo;
 [loop] for(uint h=n/2;h>0;) {--h;heapHole(lo,h,n,data[lo+h]);}
 [loop] for(uint end=n-1;end>0;--end) {uint4 v=data[lo+end];data[lo+end]=data[lo];heapHole(lo,0,end,v);}
}
void enqueue(uint lo,uint hi,uint budget) {
 if(hi-lo<2)return;
 uint slot;InterlockedAdd(counts[1-source],1,slot);
 if(slot>=capacity){InterlockedOr(counts[2],1);return;}
 if(source==0)tasks1[slot]=uint3(lo,hi,budget);else tasks0[slot]=uint3(lo,hi,budget);
}
[numthreads(64,1,1)] void work(uint3 id:SV_DispatchThreadID) {
 if(id.x>=counts[source] || id.x>=capacity)return;
 uint3 t=source==0?tasks0[id.x]:tasks1[id.x];
 if(t.x>t.y || t.y>count){InterlockedOr(counts[2],2);return;}
 if(t.y-t.x<=32){insertion(t.x,t.y);return;}
 if(t.z==0){heap(t.x,t.y);return;}
 uint2 split=partition(t.x,t.y);uint budget=(t.z>>1)+(t.z>>2);
 enqueue(t.x,split.x,budget);enqueue(split.y,t.y,budget);
}
[numthreads(1,1,1)] void init(uint3 id:SV_DispatchThreadID) {
 counts[0]=1;counts[1]=0;counts[2]=0;counts[3]=0;
 tasks0[0]=uint3(0,count,ideal);args.Store3(0,uint3(1,1,1));
}
[numthreads(1,1,1)] void advance(uint3 id:SV_DispatchThreadID) {
 uint n=counts[1-source];counts[source]=0;
 args.Store3(0,uint3((min(n,capacity)+63)/64,1,1));
}
