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
struct Row {uint64_t frame,parent_ns,loop_ns,flush_inside_ns,flush_all_ns,prepare_ns;uint32_t records,words,groups,pass,forward,stencil,grid_gate;};
inline std::mutex mutex;inline FILE* log{};inline bool attempted{};inline unsigned rows{};inline std::atomic<bool> captured{false};
inline bool enabled(){static bool v=std::getenv("ARC_PACK_PROBE")!=nullptr;return v;}
inline bool prepare(){static bool v=[]{auto p=std::getenv("ARC_PACK_PREPARE");return p&&std::atoi(p)==1;}();return v;}
inline void write(const Row& r){
 std::lock_guard<std::mutex> guard(mutex);
 if(rows++>=2048)return; // a full 2048-row log is conservatively considered truncated
 if(!attempted){attempted=true;if(auto p=std::getenv("ARC_PACK_PROBE")){fopen_s(&log,p,"wb");if(log)std::fprintf(log,"frame,parent_ns,loop_ns,flush_inside_ns,flush_all_ns,prepare_ns,records,words,groups,pass,forward,stencil,grid_gate\n");}}
 if(log){std::fprintf(log,"%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u\n",r.frame,r.parent_ns,r.loop_ns,r.flush_inside_ns,r.flush_all_ns,r.prepare_ns,r.records,r.words,r.groups,r.pass,r.forward,r.stencil,r.grid_gate);std::fflush(log);}
}
struct Scope {
 bool active=enabled();Row row{};uint64_t start{},loop_start{},inside_start{};
 Scope(uint64_t frame,uint32_t count,uint32_t pass,bool grid_gate){if(active){row.frame=frame;row.records=count;row.pass=pass;row.grid_gate=grid_gate;start=ns();}}
 void begin(){if(active){inside_start=row.flush_all_ns;loop_start=ns();}}
 void end(uint32_t words){if(active){row.loop_ns=ns()-loop_start;row.flush_inside_ns=row.flush_all_ns-inside_start;row.words=words;}}
 ~Scope(){if(active){row.parent_ns=ns()-start;write(row);}}
};
struct Flush {Scope& scope;uint64_t start{};Flush(Scope& s,bool nonempty):scope(s){if(scope.active){start=ns();scope.row.groups+=nonempty;}}~Flush(){if(scope.active)scope.row.flush_all_ns+=ns()-start;}};
template<class Queue,class Scene> void pack(Scope& scope,const Queue& queue,const Scene& scene,const void* output){
 if(!scope.active||!prepare()||scope.row.forward||scope.row.records>131072)return;
 auto start=ns();std::vector<Input> inputs;inputs.reserve(queue.batches.size());
 for(const auto& b:queue.batches){auto& o=scene.objects[b.GetInstanceIndex()];Input v;std::memcpy(v.record,&b,16);v.transparency=o.GetTransparency();v.fade=o.fadeDistance;v.radius=o.radius;v.alpha=o.alphaRef;v.lod=o.lod;v.stencil=o.userStencilRef;v.pad0=v.pad1=0;inputs.push_back(v);}
 scope.row.prepare_ns=ns()-start;
 // A single evidence packet; disk I/O and reading the mapped output are outside
 // both reported compute and preparation intervals. They are not production work.
 if(scope.row.records>=1024&&!captured.exchange(true)){
  if(auto p=std::getenv("ARC_PACK_PROBE")){
   std::string path=p;FILE* f{};fopen_s(&f,(path+".input.bin").c_str(),"wb");if(f){std::fwrite(inputs.data(),sizeof(Input),inputs.size(),f);std::fclose(f);}
   fopen_s(&f,(path+".output.bin").c_str(),"wb");if(f){std::fwrite(output,4,scope.row.words,f);std::fclose(f);}
  }
 }
}
}
