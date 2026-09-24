#pragma once
#include "wiScene.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <unordered_set>
extern "C" void ARCWickedCpuSample(const char*,double) noexcept;
namespace arc_resident_probe {
inline bool enabled(){static bool v=[] {auto p=std::getenv("ARC_RESIDENT_SCREEN");return p&&std::atoi(p)==1;}();return v;}
struct Counter { std::atomic<uint64_t> ns{0},maximum{0},calls{0}; };
inline Counter build,sort,consume;
struct Scope {
    Counter* c{};std::chrono::steady_clock::time_point start;
    explicit Scope(Counter& x){if(enabled()){c=&x;start=std::chrono::steady_clock::now();}}
    ~Scope(){if(c){auto n=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());c->ns.fetch_add(n);c->calls.fetch_add(1);auto old=c->maximum.load();while(old<n&&!c->maximum.compare_exchange_weak(old,n)){} }}
};
inline void report(){
    if(!enabled())return;
    ARCWickedCpuSample("Resident previous build elapsed sum ms",double(build.ns.exchange(0))/1e6);
    ARCWickedCpuSample("Resident previous sort elapsed sum ms",double(sort.ns.exchange(0))/1e6);
    ARCWickedCpuSample("Resident previous RenderMeshes elapsed sum ms",double(consume.ns.exchange(0))/1e6);
    ARCWickedCpuSample("Resident previous RenderMeshes max pass ms",double(consume.maximum.exchange(0))/1e6);
    ARCWickedCpuSample("Resident previous RenderMeshes calls",double(consume.calls.exchange(0)));
    static bool captured=false;auto& scene=wi::scene::GetScene();if(captured||scene.objects.GetCount()<1000)return;captured=true;
    std::unordered_set<uint64_t> meshes;std::unordered_set<uint32_t> sorts,filters,lods;
    uint64_t fade=0,alpha=0,stencil=0,special=0;
    for(size_t i=0;i<scene.objects.GetCount();++i){auto& o=scene.objects[i];meshes.insert(o.meshID);sorts.insert(o.sort_bits);filters.insert(o.GetFilterMask());lods.insert(o.lod);fade+=o.fadeDistance!=std::numeric_limits<float>::max();alpha+=o.alphaRef!=1||o.color.w!=1;stencil+=o.userStencilRef!=0;special+=o.IsForeground()||o.IsNotVisibleInMainCamera()||o.IsNotVisibleInReflections()||!o.IsRenderable();}
    ARCWickedCpuSample("Resident screen objects",double(scene.objects.GetCount()));
    ARCWickedCpuSample("Resident screen mesh references",double(meshes.size()));
    ARCWickedCpuSample("Resident screen meshes",double(scene.meshes.GetCount()));
    ARCWickedCpuSample("Resident screen materials",double(scene.materials.GetCount()));
    ARCWickedCpuSample("Resident screen sort classes",double(sorts.size()));
    ARCWickedCpuSample("Resident screen filter classes",double(filters.size()));
    ARCWickedCpuSample("Resident screen lod classes",double(lods.size()));
    ARCWickedCpuSample("Resident screen finite fades",double(fade));
    ARCWickedCpuSample("Resident screen alpha cases",double(alpha));
    ARCWickedCpuSample("Resident screen stencil cases",double(stencil));
    ARCWickedCpuSample("Resident screen special cases",double(special));
}
}
