// Bounded external CPU observation. No injection, replacement or symbol-based selection.
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <iostream>
#include <iomanip>
struct Handle{HANDLE h{};Handle(HANDLE v=nullptr):h(v){}~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle(const Handle&)=delete;};
using Clock=std::chrono::steady_clock;
static unsigned long long ticks(FILETIME v){return (unsigned long long(v.dwHighDateTime)<<32)|v.dwLowDateTime;}
static unsigned long long cpu(HANDLE h){FILETIME a,b,c,d;return GetThreadTimes(h,&a,&b,&c,&d)?ticks(c)+ticks(d):0;}
static std::string utf8(const wchar_t* s){int n=WideCharToMultiByte(CP_UTF8,0,s,-1,nullptr,0,nullptr,nullptr);std::string r(n,'\0');WideCharToMultiByte(CP_UTF8,0,s,-1,r.data(),n,nullptr,nullptr);r.pop_back();return r;}
static void quoted(std::ostream& out,const std::string& s){out<<'"';for(char c:s){if(c=='"')out<<'"';out<<c;}out<<'"';}
struct WindowThread{DWORD pid{},tid{};long long area{};};
static BOOL CALLBACK find_window(HWND window,LPARAM data){auto& result=*reinterpret_cast<WindowThread*>(data);DWORD pid{};auto tid=GetWindowThreadProcessId(window,&pid);RECT rect{};if(pid==result.pid&&IsWindowVisible(window)&&GetClientRect(window,&rect)){auto area=long long(rect.right-rect.left)*(rect.bottom-rect.top);if(area>result.area){result.tid=tid;result.area=area;}}return TRUE;}
struct Thread{DWORD id{};HANDLE h{};unsigned long long cycles{},time{};};
// Exactly undo our own suspension, including when GetThreadContext fails.
struct Suspension{HANDLE h;bool valid;Suspension(HANDLE x):h(x),valid(SuspendThread(x)!=DWORD(-1)){}~Suspension(){if(valid)ResumeThread(h);}};
int wmain(int argc,wchar_t** argv){
 if(argc!=4)return 2;const DWORD pid=wcstoul(argv[1],nullptr,10);const unsigned seconds=wcstoul(argv[2],nullptr,10);if(!pid||seconds<1||seconds>12)return 2;
 const auto output=std::filesystem::absolute(argv[3]);if(std::filesystem::exists(output))return 3;std::filesystem::create_directories(output);
 Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid));if(!process.h)return 4;
 std::ofstream modules(output/L"modules.csv");modules<<"base,size,path,main\n";Handle inventory(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));MODULEENTRY32W mod{};mod.dwSize=sizeof(mod);bool first=true;
 if(Module32FirstW(inventory.h,&mod))do{
  modules<<std::hex<<uintptr_t(mod.modBaseAddr)<<','<<std::dec<<mod.modBaseSize<<',';quoted(modules,utf8(mod.szExePath));modules<<','<<first<<'\n';
  if(first){
   std::error_code copy_error;std::filesystem::copy_file(mod.szExePath,output/L"module-image.bin",std::filesystem::copy_options::none,copy_error);if(copy_error)return 6;
   IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};SIZE_T got{};
   if(ReadProcessMemory(process.h,mod.modBaseAddr,&dos,sizeof(dos),&got)&&dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>0&&dos.e_lfanew<1024*1024&&ReadProcessMemory(process.h,mod.modBaseAddr+dos.e_lfanew,&nt,sizeof(nt),&got)&&nt.Signature==IMAGE_NT_SIGNATURE){
    std::ofstream map(output/L"code-sections.csv");map<<"rva,size,file,read_complete\n";
    for(unsigned i=0;i<std::min<unsigned>(nt.FileHeader.NumberOfSections,96);++i){IMAGE_SECTION_HEADER section{};auto address=mod.modBaseAddr+dos.e_lfanew+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader+i*sizeof(section);
     if(!ReadProcessMemory(process.h,address,&section,sizeof(section),&got)||!(section.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
     const auto bytes=section.Misc.VirtualSize;if(!bytes||bytes>16*1024*1024||uint64_t(section.VirtualAddress)+bytes>mod.modBaseSize)continue;
     std::vector<char> data(bytes);bool read=ReadProcessMemory(process.h,mod.modBaseAddr+section.VirtualAddress,data.data(),bytes,&got)&&got==bytes;
     const auto file="code-"+std::to_string(i)+".bin";if(read){std::ofstream bin(output/file,std::ios::binary);bin.write(data.data(),data.size());}
     map<<section.VirtualAddress<<','<<bytes<<','<<file<<','<<read<<'\n';
    }
   }
  }
  first=false;
 }while(Module32NextW(inventory.h,&mod));modules.close();
 std::map<DWORD,Thread> threads;std::ofstream samples(output/L"samples.csv"),activity(output/L"thread-activity.csv");
 samples<<"elapsed_ms,tid,interval_cycles,interval_cpu_100ns,suspend_us,rip,rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15,eflags\n";activity<<"elapsed_ms,tid,cycles,cpu_100ns\n";
 WindowThread window_thread{pid};EnumWindows(find_window,reinterpret_cast<LPARAM>(&window_thread));
 unsigned probes{},failed{},refreshes{},slow{};double suspension_us{};const auto started=Clock::now();bool context_enabled=true;
 while(std::chrono::duration<double>(Clock::now()-started).count()<seconds&&WaitForSingleObject(process.h,0)==WAIT_TIMEOUT){
  const auto cycle=Clock::now();double elapsed=std::chrono::duration<double,std::milli>(cycle-started).count();
  if(refreshes++%10==0){Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0));THREADENTRY32 e{};e.dwSize=sizeof(e);if(Thread32First(snap.h,&e))do{if(e.th32OwnerProcessID!=pid||threads.count(e.th32ThreadID)||threads.size()>=256)continue;HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,e.th32ThreadID);if(h){ULONG64 c{};QueryThreadCycleTime(h,&c);threads.emplace(e.th32ThreadID,Thread{e.th32ThreadID,h,c,cpu(h)});}}while(Thread32Next(snap.h,&e));}
  struct Active{Thread* t;unsigned long long cycles,time;};std::vector<Active> ranked;
  for(auto& [id,t]:threads){ULONG64 value{};if(!QueryThreadCycleTime(t.h,&value)||value<t.cycles)continue;auto time=cpu(t.h);auto delta=value-t.cycles;auto dt=time>=t.time?time-t.time:0;t.cycles=value;t.time=time;if(delta){ranked.push_back({&t,delta,dt});activity<<elapsed<<','<<id<<','<<delta<<','<<dt<<'\n';}}
  std::sort(ranked.begin(),ranked.end(),[](auto& a,auto& b){return a.cycles>b.cycles;});
  // Window owner is a candidate critical-path thread, not a proof. Include it
  // independently of worker spin activity; do not use any game symbol or address.
  auto ui=std::find_if(ranked.begin(),ranked.end(),[&](auto& x){return x.t->id==window_thread.tid;});if(ui!=ranked.end())std::rotate(ranked.begin(),ui,ui+1);
  if(context_enabled)for(unsigned i=0;i<std::min<size_t>(4,ranked.size());++i){auto& a=ranked[i];CONTEXT c{};c.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;bool ok=false;auto before=Clock::now();{Suspension pause(a.t->h);if(pause.valid)ok=GetThreadContext(a.t->h,&c)!=FALSE;}const double us=std::chrono::duration<double,std::micro>(Clock::now()-before).count();suspension_us+=us;++probes;if(!ok){++failed;continue;}
   samples<<std::setprecision(12)<<elapsed<<','<<a.t->id<<','<<a.cycles<<','<<a.time<<','<<us<<std::hex<<','<<c.Rip;
   for(auto v:std::array<DWORD64,16>{c.Rax,c.Rcx,c.Rdx,c.Rbx,c.Rsp,c.Rbp,c.Rsi,c.Rdi,c.R8,c.R9,c.R10,c.R11,c.R12,c.R13,c.R14,c.R15})samples<<','<<v;samples<<','<<c.EFlags<<std::dec<<'\n';
   // Budget backstop: no continued context capture after a slow suspension.
   if(us>5000||suspension_us>50000){++slow;context_enabled=false;break;}
  }
  auto spent=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-cycle).count();if(spent<100)Sleep(DWORD(100-spent));
 }
 for(auto& [id,t]:threads)CloseHandle(t.h);
 std::ofstream meta(output/L"probe.json");meta<<"{\"schema\":1,\"pid\":"<<pid<<",\"seconds\":"<<std::chrono::duration<double>(Clock::now()-started).count()<<",\"context_probes\":"<<probes<<",\"failed\":"<<failed<<",\"slow_suspensions\":"<<slow<<",\"suspension_us\":"<<suspension_us<<",\"thread_limit\":256,\"context_threads_per_tick\":4,\"sampling_period_ms\":100,\"window_thread_hint\":"<<window_thread.tid<<",\"window_thread_is_critical_path_proof\":false,\"instruction_trace\":false,\"memory_dependencies_proven\":false,\"replacement_enabled\":false}";
 return samples&&activity&&meta?0:5;
}
