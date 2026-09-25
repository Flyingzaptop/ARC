#pragma once
// SOURCE-ASSISTED control instrumentation, not an ARC game adapter.
// Timings isolate the original outer record loop and its nested batch_flush.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <string>
#include <cstdint>
#include <windows.h>
namespace arc_pack_probe {
using Clock=std::chrono::steady_clock;
inline uint64_t ns(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());}
struct Input {uint32_t record[4];float transparency,fade,radius,alpha;uint32_t lod,stencil,pad0,pad1;};
static_assert(sizeof(Input)==48);
struct Row {uint64_t frame,parent_ns,loop_ns,flush_inside_ns,flush_all_ns,prepare_ns,allocation_ns,gather_ns,release_ns,record_copy_ns;uint32_t records,words,groups,pass,forward,stencil,grid_gate,reuse,pool_skip;};
inline std::mutex mutex;inline FILE* log{};inline bool attempted{};inline unsigned rows{};inline std::atomic<bool> captured{false};
inline bool enabled(){static bool v=std::getenv("ARC_PACK_PROBE")!=nullptr;return v;}
inline bool prepare(){static bool v=[]{auto p=std::getenv("ARC_PACK_PREPARE");return p&&std::atoi(p)==1;}();return v;}
inline void write(const Row& r){
 std::lock_guard<std::mutex> guard(mutex);
 if(rows++>=2048)return; // a full 2048-row log is conservatively considered truncated
 if(!attempted){attempted=true;if(auto p=std::getenv("ARC_PACK_PROBE")){fopen_s(&log,p,"wb");if(log)std::fprintf(log,"frame,parent_ns,loop_ns,flush_inside_ns,flush_all_ns,prepare_ns,records,words,groups,pass,forward,stencil,grid_gate,allocation_ns,gather_ns,release_ns,record_copy_ns,reuse,pool_skip\n");}}
 if(log){std::fprintf(log,"%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%llu,%llu,%u,%u\n",r.frame,r.parent_ns,r.loop_ns,r.flush_inside_ns,r.flush_all_ns,r.prepare_ns,r.records,r.words,r.groups,r.pass,r.forward,r.stencil,r.grid_gate,r.allocation_ns,r.gather_ns,r.release_ns,r.record_copy_ns,r.reuse,r.pool_skip);std::fflush(log);}
}
struct Scope {
 bool active=enabled();Row row{};uint64_t start{},loop_start{},inside_start{};
 Scope(uint64_t frame,uint32_t count,uint32_t pass,bool grid_gate){if(active){row.frame=frame;row.records=count;row.pass=pass;row.grid_gate=grid_gate;start=ns();}}
 void begin(){if(active){inside_start=row.flush_all_ns;loop_start=ns();}}
 void end(uint32_t words){if(active){row.loop_ns=ns()-loop_start;row.flush_inside_ns=row.flush_all_ns-inside_start;row.words=words;}}
 ~Scope(){if(active){row.parent_ns=ns()-start;write(row);}}
};
struct Flush {Scope& scope;uint64_t start{};Flush(Scope& s,bool nonempty):scope(s){if(scope.active){start=ns();scope.row.groups+=nonempty;}}~Flush(){if(scope.active)scope.row.flush_all_ns+=ns()-start;}};
struct PoolSlot {std::atomic_flag used=ATOMIC_FLAG_INIT;std::vector<Input> values;std::vector<unsigned char> records;};
inline PoolSlot pool[4];
inline bool reuse(){static bool v=[]{auto p=std::getenv("ARC_PACK_REUSE");return p&&std::atoi(p)==1;}();return v;}
inline void* (__cdecl* volatile copy_call)(void*,const void*,size_t)=&std::memcpy;
inline volatile unsigned char copy_sink{};
template<class Queue,class Scene> void pack(Scope& scope,const Queue& queue,const Scene& scene,const void* output){
 if(!scope.active||!prepare()||scope.row.forward||scope.row.records>131072)return;
 scope.row.reuse=reuse();PoolSlot* slot=nullptr;
 for(auto& s:pool)if(!s.used.test_and_set()){slot=&s;break;}if(!slot){scope.row.pool_skip=1;return;}
 struct Release {PoolSlot* slot;~Release(){if(slot)slot->used.clear();}} release{slot};
 std::vector<Input> fresh;auto& inputs=reuse()?slot->values:fresh;
 auto start=ns();inputs.clear();inputs.reserve(queue.batches.size());scope.row.allocation_ns=ns()-start;
 start=ns();
 for(const auto& b:queue.batches){auto& o=scene.objects[b.GetInstanceIndex()];Input v;std::memcpy(v.record,&b,16);v.transparency=o.GetTransparency();v.fade=o.fadeDistance;v.radius=o.radius;v.alpha=o.alphaRef;v.lod=o.lod;v.stencil=o.userStencilRef;v.pad0=v.pad1=0;inputs.push_back(v);}
 scope.row.gather_ns=ns()-start;scope.row.prepare_ns=scope.row.allocation_ns+scope.row.gather_ns;
 slot->records.resize(queue.batches.size()*16);
 start=ns();copy_call(slot->records.data(),queue.batches.data(),queue.batches.size()*16);scope.row.record_copy_ns=ns()-start;
 copy_sink=slot->records.back();
 // A single evidence packet; disk I/O and reading the mapped output are outside
 // both reported compute and preparation intervals. They are not production work.
 if(scope.row.records>=1024&&!captured.exchange(true)){
  if(auto p=std::getenv("ARC_PACK_PROBE")){
   std::string path=p;FILE* f{};fopen_s(&f,(path+".input.bin").c_str(),"wb");if(f){std::fwrite(inputs.data(),sizeof(Input),inputs.size(),f);std::fclose(f);}
   fopen_s(&f,(path+".output.bin").c_str(),"wb");if(f){std::fwrite(output,4,scope.row.words,f);std::fclose(f);}
  }
 }
 if(!reuse()){auto t=ns();std::vector<Input>().swap(inputs);scope.row.release_ns=ns()-t;}
}
}
