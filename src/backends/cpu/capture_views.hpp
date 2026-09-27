#pragma once
// Bounded training snapshots of VM regions actually touched by the observed path.
// Not an ownership certificate and never a replacement path.
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
namespace arc::cpu {
struct ViewSnapshots {
 struct Region{uintptr_t base{};size_t size{},offset{};DWORD type{},protect{};bool before{},after{};};
 Region regions[32];unsigned count{},missed{};size_t budget{},used{};unsigned char* storage{};CONTEXT entry{},exit{};uintptr_t teb{};bool completed{};
 bool initialize(size_t bytes){budget=bytes;storage=(unsigned char*)VirtualAlloc(nullptr,2*budget,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);return storage!=nullptr;}
 void begin(const CONTEXT& c){entry=c;entry.EFlags&=~0x10100u;teb=uintptr_t(NtCurrentTeb());}
 void observe(uintptr_t address){
  for(unsigned i=0;i<count;++i)if(address>=regions[i].base&&address-regions[i].base<regions[i].size)return;
  MEMORY_BASIC_INFORMATION m{};
  if(count>=32||VirtualQuery((void*)address,&m,sizeof(m))!=sizeof(m)||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS))||!m.RegionSize||m.RegionSize>64ull*1024*1024||m.RegionSize>budget-used){++missed;return;}
  auto& r=regions[count++];r.base=uintptr_t(m.BaseAddress);r.size=m.RegionSize;r.offset=used;r.type=m.Type;r.protect=m.Protect;SIZE_T got{};r.before=ReadProcessMemory(GetCurrentProcess(),m.BaseAddress,storage+used,r.size,&got)&&got==r.size;used+=r.size;
 }
 void finish(const CONTEXT& c){exit=c;exit.EFlags&=~0x10100u;for(unsigned i=0;i<count;++i){auto& r=regions[i];SIZE_T got{};r.after=ReadProcessMemory(GetCurrentProcess(),(void*)r.base,storage+budget+r.offset,r.size,&got)&&got==r.size;}completed=true;}
 void save(const std::filesystem::path& directory,uintptr_t module,uintptr_t first,uintptr_t last){
  std::ofstream(directory/L"entry-context.bin",std::ios::binary).write((char*)&entry,sizeof(entry));std::ofstream(directory/L"exit-context.bin",std::ios::binary).write((char*)&exit,sizeof(exit));
  std::ofstream f(directory/L"views.json");f<<"{\"schema\":1,\"module_base\":"<<module<<",\"loop_entry_rva\":"<<first-module<<",\"loop_end_rva\":"<<last-module<<",\"teb\":"<<teb<<",\"completed\":"<<(completed?"true":"false")<<",\"missed_regions\":"<<missed<<",\"allocated_bytes\":"<<2*budget<<",\"captured_bytes_per_image\":"<<used<<",\"entry_context_file\":\"entry-context.bin\",\"exit_context_file\":\"exit-context.bin\",\"regions\":[";
  for(unsigned i=0;i<count;++i){auto& r=regions[i];auto before="region-"+std::to_string(i)+"-before.bin",after="region-"+std::to_string(i)+"-after.bin";if(r.before)std::ofstream(directory/before,std::ios::binary).write((char*)storage+r.offset,r.size);if(r.after)std::ofstream(directory/after,std::ios::binary).write((char*)storage+budget+r.offset,r.size);if(i)f<<',';f<<"{\"base\":"<<r.base<<",\"size\":"<<r.size<<",\"type\":"<<r.type<<",\"protect\":"<<r.protect<<",\"before_ok\":"<<(r.before?"true":"false")<<",\"after_ok\":"<<(r.after?"true":"false")<<",\"before_file\":\""<<before<<"\",\"after_file\":\""<<after<<"\"}";}f<<"],\"coherent_snapshot_proven\":false,\"replacement_allowed\":false}";
 }
};
}
