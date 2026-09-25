#pragma once
#include <windows.h>
#include <vector>
#include <algorithm>
#include <fstream>
#include <string>
#include <cstdint>
namespace arc_memory_windows {
// Generic inventory; no engine type, field, object address or record stride.
inline void capture(const std::string& prefix,uint64_t frame,const std::vector<const void*>& excluded){
 struct Region{uintptr_t address;size_t bytes;};std::vector<Region> regions;
 uintptr_t address=0;MEMORY_BASIC_INFORMATION m{};
 while(VirtualQuery((void*)address,&m,sizeof(m))==sizeof(m)){
  auto base=(uintptr_t)m.BaseAddress;auto size=m.RegionSize;if(base+size<=address)break;address=base+size;
  if(m.State!=MEM_COMMIT||m.Type!=MEM_PRIVATE||(m.Protect&0xff)!=PAGE_READWRITE||(m.Protect&PAGE_GUARD)||size<(1u<<20)||size>(128u<<20))continue;
  bool skip=false;for(auto p:excluded)if((uintptr_t)p>=base&&(uintptr_t)p<base+size)skip=true;
  if(!skip)regions.push_back({base,size});
 }
 std::sort(regions.begin(),regions.end(),[](auto a,auto b){return a.bytes!=b.bytes?a.bytes>b.bytes:a.address<b.address;});
 std::ofstream manifest(prefix+".windows.json");manifest<<"{\"frame\":"<<frame<<",\"eligible_regions\":"<<regions.size()<<",\"regions\":[";
 unsigned written=0;for(auto r:regions){if(written==64)break;size_t n=std::min(r.bytes,size_t(256*1024));std::vector<char> data(n);SIZE_T got{};if(!ReadProcessMemory(GetCurrentProcess(),(void*)r.address,data.data(),n,&got)||got!=n)continue;
  std::string path=prefix+".cpu"+std::to_string(written)+".bin";std::ofstream file(path,std::ios::binary);file.write(data.data(),n);if(!file)throw std::runtime_error("CPU snapshot write");if(written)manifest<<',';manifest<<"{\"address\":"<<r.address<<",\"region_bytes\":"<<r.bytes<<",\"bytes\":"<<n<<",\"file\":\""<<path.substr(path.find_last_of("/\\")+1)<<"\"}";++written;
 }
 manifest<<"],\"capture_kind\":\"generic_private_rw_windows\",\"max_regions\":64,\"window_bytes\":262144}";
}
}
