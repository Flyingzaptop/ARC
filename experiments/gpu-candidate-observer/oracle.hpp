#pragma once
// Independent checker only. No oracle file is an input to selection or discovery.
#include "wiScene.h"
#include <fstream>
#include <mutex>
extern "C" uint64_t ARCAuditNativeResource(const wi::graphics::GPUResource*);
namespace arc_candidate_oracle {
inline void observe(const wi::scene::Scene& s){char path[2048]{};if(!GetEnvironmentVariableA("ARC_GPU_CANDIDATES",path,2048)||s.objects.GetCount()<2||!s.instanceBuffer.IsValid())return;static std::mutex mutex;std::lock_guard<std::mutex> lock(mutex);static unsigned count{};if(count++>=2048)return;static std::ofstream f(std::string(path)+"/oracle.jsonl");f<<"{\"native_identity\":"<<ARCAuditNativeResource(&s.instanceBuffer)<<",\"cpu_address\":"<<(uintptr_t)&s.objects[0].center<<",\"cpu_stride\":"<<((uintptr_t)&s.objects[1]-(uintptr_t)&s.objects[0])<<",\"gpu_offset\":"<<offsetof(ShaderMeshInstance,center)<<",\"gpu_stride\":"<<sizeof(ShaderMeshInstance)<<",\"width\":12}\n";f.flush();}
}
