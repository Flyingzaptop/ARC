// Bounded training watch. Hardware data breakpoint; never changes application bytes.
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <vector>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <algorithm>
namespace {
struct Event{CONTEXT c;LONGLONG qpc;DWORD tid;CONTEXT callers[3];unsigned caller_count;bool read;unsigned char data[32];};
static Event events[32];static std::atomic<unsigned> count{},inside{};static std::atomic<bool> stop{};
static bool readwrite{};static uintptr_t target{};static unsigned width{},limit{},waitMs{};static std::filesystem::path out;
static std::vector<DWORD> armed;static void* veh{};static LONGLONG frequency{},began{};
using GetStatus=LONG(NTAPI*)();using SetStatus=void(NTAPI*)(LONG);static GetStatus getStatus;static SetStatus setStatus;
LONGLONG now(){LARGE_INTEGER n;QueryPerformanceCounter(&n);return n.QuadPart;}
LONG CALLBACK handler(EXCEPTION_POINTERS* p){auto* c=p->ContextRecord;if(p->ExceptionRecord->ExceptionCode!=EXCEPTION_SINGLE_STEP||!(c->Dr6&1)||c->Dr0!=target)return EXCEPTION_CONTINUE_SEARCH;
 DWORD error=GetLastError();LONG status=getStatus();inside.fetch_add(1);
 if(!stop){auto i=count.fetch_add(1);if(i<limit){auto& e=events[i];e.c=*c;e.tid=GetCurrentThreadId();e.qpc=now();
  CONTEXT walk=*c;for(unsigned depth=0;depth<3;++depth){DWORD64 base{};auto fn=RtlLookupFunctionEntry(walk.Rip,&base,nullptr);if(!fn)break;PVOID data{};DWORD64 frame{};RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,walk.Rip,fn,&walk,&data,&frame,nullptr);if(!walk.Rip)break;e.callers[e.caller_count++]=walk;}
SIZE_T n{};e.read=ReadProcessMemory(GetCurrentProcess(),(void*)target,e.data,width,&n)&&n==width;}if(i+1>=limit)stop=true;}
 if(stop){c->Dr0=0;c->Dr7&=~DWORD64(0xf0003);}c->Dr6&=~DWORD64(1);inside.fetch_sub(1);setStatus(status);SetLastError(error);return EXCEPTION_CONTINUE_EXECUTION;
}
bool configure(DWORD tid,bool enable){HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,tid);if(!h)return false;bool ok=false;DWORD suspended=SuspendThread(h);if(suspended!=DWORD(-1)){CONTEXT c{};c.ContextFlags=CONTEXT_DEBUG_REGISTERS;
 if(GetThreadContext(h,&c)){if(enable){if(!(c.Dr7&255)&&!c.Dr0&&!c.Dr1&&!c.Dr2&&!c.Dr3){c.Dr0=target;c.Dr7=(c.Dr7&~DWORD64(0xf0003))|(readwrite?0xf0001:0xd0001);ok=SetThreadContext(h,&c)!=FALSE;}}else if(c.Dr0==target){c.Dr0=0;c.Dr7&=~DWORD64(0xf0003);c.Dr6&=~DWORD64(1);ok=SetThreadContext(h,&c)!=FALSE;}else if(!enable)ok=true;}
 ResumeThread(h);}CloseHandle(h);return ok;}
void hex(std::ostream& f,const void* data,size_t n){auto b=(const unsigned char*)data;for(size_t i=0;i<n;++i)f<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b[i]);f<<std::dec;}
DWORD WINAPI worker(void*){LARGE_INTEGER f;QueryPerformanceFrequency(&f);frequency=f.QuadPart;began=now();veh=AddVectoredExceptionHandler(1,handler);if(!veh)return 1;
 HANDLE list=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 t{};t.dwSize=sizeof(t);if(Thread32First(list,&t))do{if(t.th32OwnerProcessID==GetCurrentProcessId()&&t.th32ThreadID!=GetCurrentThreadId()&&armed.size()<128&&configure(t.th32ThreadID,true))armed.push_back(t.th32ThreadID);}while(Thread32Next(list,&t));CloseHandle(list);
 {std::ofstream ready(out/L"ready.json");ready<<"{\"pid\":"<<GetCurrentProcessId()<<",\"qpc\":"<<now()<<",\"armed_threads\":"<<armed.size()<<"}";}
 while(!stop&&double(now()-began)*1000/frequency<waitMs){if(std::filesystem::exists(out/L"cancel"))stop=true;Sleep(1);}stop=true;
 unsigned restored=0;for(auto tid:armed)restored+=configure(tid,false);while(inside.load())Sleep(1);
 unsigned n=std::min(count.load(),limit);std::ofstream file(out/L"writes.jsonl");
 for(unsigned i=0;i<n;++i){auto& e=events[i];auto& c=e.c;DWORD64 regs[]={c.Rax,c.Rcx,c.Rdx,c.Rbx,c.Rsp,c.Rbp,c.Rsi,c.Rdi,c.R8,c.R9,c.R10,c.R11,c.R12,c.R13,c.R14,c.R15};DWORD64 base{};auto* fn=RtlLookupFunctionEntry(c.Rip-1,&base,nullptr);uintptr_t begin=fn?base+fn->BeginAddress:0,end=fn?base+fn->EndAddress:0;
  std::vector<char> code;if(begin&&end>begin&&end-begin<=65536){code.resize(end-begin);SIZE_T got{};if(!ReadProcessMemory(GetCurrentProcess(),(void*)begin,code.data(),code.size(),&got)||got!=code.size())code.clear();}
  if(!code.empty()){std::ofstream binary(out/(L"function-"+std::to_wstring(i)+L".bin"),std::ios::binary);binary.write(code.data(),code.size());}
  {std::ofstream parents(out/(L"callers-"+std::to_wstring(i)+L".json"));parents<<"[";
   for(unsigned j=0;j<e.caller_count;++j){auto& parent=e.callers[j];DWORD64 pb{};auto pf=RtlLookupFunctionEntry(parent.Rip-1,&pb,nullptr);uintptr_t start=pf?pb+pf->BeginAddress:0,finish=pf?pb+pf->EndAddress:0;std::vector<char> bytes;if(start&&finish>start&&finish-start<=65536){bytes.resize(finish-start);SIZE_T got{};if(!ReadProcessMemory(GetCurrentProcess(),(void*)start,bytes.data(),bytes.size(),&got)||got!=bytes.size())bytes.clear();}if(!bytes.empty()){std::ofstream binary(out/(L"caller-"+std::to_wstring(i)+L"-"+std::to_wstring(j)+L".bin"),std::ios::binary);binary.write(bytes.data(),bytes.size());}if(j)parents<<',';parents<<"{\"depth\":"<<j<<",\"return_pc\":"<<parent.Rip<<",\"stack_pointer\":"<<parent.Rsp<<",\"begin\":"<<start<<",\"end\":"<<finish<<",\"bytes\":"<<bytes.size()<<",\"volatile_registers_not_reconstructed\":true}";}
   parents<<"]";}
  file<<"{\"event\":"<<i<<",\"qpc\":"<<e.qpc<<",\"tid\":"<<e.tid<<",\"rip_after\":"<<c.Rip<<",\"target\":"<<target<<",\"bytes\":"<<width<<",\"read_ok\":"<<(e.read?"true":"false")<<",\"value\":\"";hex(file,e.data,width);file<<"\",\"registers\":[";for(unsigned j=0;j<16;++j){if(j)file<<',';file<<regs[j];}file<<"],\"xmm\":\"";hex(file,&c.Xmm0,256);file<<"\",\"mxcsr\":"<<c.MxCsr<<",\"module_base\":"<<base<<",\"function_begin\":"<<begin<<",\"function_end\":"<<end<<",\"code_bytes\":"<<code.size()<<"}\n";
 }
 std::ofstream done(out/L"done.json");done<<"{\"events\":"<<n<<",\"event_limit\":"<<limit<<",\"wait_ms\":"<<waitMs<<",\"qpc_frequency\":"<<frequency<<",\"elapsed_ms\":"<<double(now()-began)*1000/frequency<<",\"armed\":"<<armed.size()<<",\"restored\":"<<restored<<",\"allocated_event_bytes\":"<<sizeof(events)<<",\"replacement_enabled\":false}";
 // Keep the DLL/handler alive: no unload race with a late exception dispatch.
 return 0;}
}
extern "C" __declspec(dllexport) DWORD WINAPI ArcInitialize(void* path){if(!path||target||IsDebuggerPresent())return 1;out=std::filesystem::path((const wchar_t*)path).parent_path();std::ifstream f(out/L"request.txt");f>>std::hex>>target>>std::dec>>width>>limit>>waitMs;if(!f||target%4||width<4||width>32||limit<1||limit>32||waitMs<1||waitMs>10000)return 2;
 std::string access;if(f>>access){if(access!="rw")return 6;readwrite=true;}
 MEMORY_BASIC_INFORMATION m{};if(!VirtualQuery((void*)target,&m,sizeof(m))||m.State!=MEM_COMMIT||m.Type!=MEM_PRIVATE||m.Protect&(PAGE_GUARD|PAGE_NOACCESS))return 3;
 auto nt=GetModuleHandleW(L"ntdll.dll");getStatus=(GetStatus)GetProcAddress(nt,"RtlGetLastNtStatus");setStatus=(SetStatus)GetProcAddress(nt,"RtlSetLastWin32ErrorAndNtStatusFromNtStatus");if(!getStatus||!setStatus)return 4;
 HANDLE h=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(!h)return 5;CloseHandle(h);return 0;}
extern "C" __declspec(dllexport) DWORD WINAPI ArcCaptureBytes(void*){return sizeof(events);}
extern "C" __declspec(dllexport) DWORD WINAPI ArcStopOptimizer(void*){stop=true;return 0;}
BOOL WINAPI DllMain(HINSTANCE h,DWORD why,void*){if(why==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(h);return TRUE;}
