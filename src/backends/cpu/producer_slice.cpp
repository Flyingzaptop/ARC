// Bounded hardware-entry/TF capture, addresses supplied by the automatic writer planner.
#include <windows.h>
#include <atomic>
#include <vector>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <cstdio>
#include <algorithm>
namespace {
struct Mem{int base,index,scale;long long disp;unsigned size;};
struct Step{uintptr_t pc;unsigned length,n;Mem mem[2];};
struct Read{uintptr_t address;unsigned size;bool ok;unsigned char bytes[32];};
struct Event{CONTEXT c;LONGLONG qpc;DWORD tid;unsigned call,kind,n,code_size;unsigned char code[15];Read memory[2];uintptr_t output;unsigned char value[32];bool value_ok;};
static Event events[512];static Step steps[65];static unsigned stepCount,limitCalls,waitMs,width;static uintptr_t entry,exitPc,target,stride,output;
static Mem destination;static std::vector<DWORD> threads,armed;static std::atomic<unsigned> count{},inside{},calls{},owner{},skipped{};static std::atomic<bool> stop{},active{};static unsigned reason{};
static std::filesystem::path folder;static LONGLONG frequency,start;static void* veh;
using GS=LONG(NTAPI*)();using SS=void(NTAPI*)(LONG);static GS getStatus;static SS setStatus;
LONGLONG now(){LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart;}
uint64_t reg(const CONTEXT& c,int i,uintptr_t next){uint64_t r[]={c.Rax,c.Rcx,c.Rdx,c.Rbx,c.Rsp,c.Rbp,c.Rsi,c.Rdi,c.R8,c.R9,c.R10,c.R11,c.R12,c.R13,c.R14,c.R15};return i<0?0:i==16?next:r[i];}
uintptr_t addr(const Mem& m,const CONTEXT& c,uintptr_t next){return reg(c,m.base,next)+reg(c,m.index,next)*m.scale+m.disp;}
bool read(uintptr_t p,void* dst,size_t bytes){SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),(void*)p,dst,bytes,&n)&&n==bytes;}
void record(CONTEXT* c,unsigned kind){unsigned i=count.fetch_add(1);if(i>=512){stop=true;reason=1;return;}auto& e=events[i];e.c=*c;e.qpc=now();e.tid=GetCurrentThreadId();e.call=calls;e.kind=kind;e.output=output;e.value_ok=read(output,e.value,width);
 if(kind!=2){auto it=std::find_if(steps,steps+stepCount,[&](const Step& s){return s.pc==c->Rip;});if(it==steps+stepCount){stop=true;reason=2;return;}e.code_size=it->length;if(!read(c->Rip,e.code,e.code_size)){stop=true;reason=3;}e.n=it->n;for(unsigned j=0;j<e.n;++j){auto& r=e.memory[j];r.address=addr(it->mem[j],*c,c->Rip+it->length);r.size=it->mem[j].size;r.ok=read(r.address,r.bytes,r.size);if(!r.ok){stop=true;reason=3;}}}
}
LONG CALLBACK handler(EXCEPTION_POINTERS* p){auto* c=p->ContextRecord;DWORD tid=GetCurrentThreadId();bool breakpoint=(c->Dr6&1)&&c->Rip==entry&&(c->Dr0==entry||(stop&&c->Dr0==0&&!(c->Dr7&3)&&std::find(threads.begin(),threads.end(),tid)!=threads.end()));bool stepping=active&&owner==tid;
 if(p->ExceptionRecord->ExceptionCode!=EXCEPTION_SINGLE_STEP){if(stepping){stop=true;reason=4;c->EFlags&=~0x100u;active=false;}return EXCEPTION_CONTINUE_SEARCH;}
 if(!breakpoint&&!stepping)return EXCEPTION_CONTINUE_SEARCH;
 DWORD error=GetLastError();LONG status=getStatus();inside.fetch_add(1);
 if(stop){if(stepping){c->EFlags&=~0x100u;active=false;owner=0;}c->Dr0=0;c->Dr7&=~DWORD64(0xf0003);c->Dr6=0;c->EFlags|=0x10000u;}
 else if(breakpoint&&!stepping){uintptr_t a=addr(destination,*c,exitPc);uint64_t index=a>=target&&stride?(a-target)/stride:~0ull;
  if(active||a<target||(a-target)%stride||!(index<32||(index>=64&&index<96))){++skipped;c->Dr6=0;c->EFlags|=0x10000u;}
  else{unsigned none=0;if(owner.compare_exchange_strong(none,tid)){if(stop){owner=0;c->Dr0=0;c->Dr7&=~DWORD64(0xf0003);c->Dr6=0;c->EFlags|=0x10000u;}else{output=a;active=true;record(c,1);c->EFlags|=0x10100u;c->Dr6=0;}}else{++skipped;c->Dr6=0;c->EFlags|=0x10000u;}}
 }else if(stepping){if(c->Rip==exitPc){record(c,2);c->EFlags&=~0x100u;if(++calls>=limitCalls)stop=true;active=false;owner=0;if(stop){c->Dr0=0;c->Dr7&=~DWORD64(0xf0003);}}else{record(c,0);if(stop){c->EFlags&=~0x100u;active=false;}else c->EFlags|=0x100u;}c->Dr6=0;}
 inside.fetch_sub(1);setStatus(status);SetLastError(error);return EXCEPTION_CONTINUE_EXECUTION;
}
bool configure(DWORD tid,bool enable){HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,tid);if(!h)return false;bool ok=false;if(SuspendThread(h)!=DWORD(-1)){CONTEXT c{};c.ContextFlags=CONTEXT_DEBUG_REGISTERS|CONTEXT_CONTROL;if(GetThreadContext(h,&c)){if(enable){if(!(c.Dr7&255)&&!(c.EFlags&0x100)&&!c.Dr0){c.Dr0=entry;c.Dr7=(c.Dr7&~DWORD64(0xf0003))|1;ok=SetThreadContext(h,&c)!=FALSE;}}else{if(c.Dr0==entry){c.Dr0=0;c.Dr7&=~DWORD64(0xf0003);}if(owner==tid)c.EFlags&=~0x100u;c.Dr6=0;ok=SetThreadContext(h,&c)!=FALSE;}}ResumeThread(h);}CloseHandle(h);return ok;}
void hex(std::ostream& f,const void* p,size_t n){auto b=(const unsigned char*)p;for(size_t i=0;i<n;++i)f<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b[i]);f<<std::dec;}
DWORD WINAPI worker(void*){LARGE_INTEGER q;QueryPerformanceFrequency(&q);frequency=q.QuadPart;start=now();veh=AddVectoredExceptionHandler(1,handler);if(!veh)return 1;for(auto tid:threads)if(configure(tid,true))armed.push_back(tid);
 while(!stop&&double(now()-start)*1000/frequency<waitMs){if(std::filesystem::exists(folder/L"cancel"))stop=true;Sleep(1);}stop=true;unsigned restored=0;for(auto tid:armed)restored+=configure(tid,false);while(inside)Sleep(1);
 std::ofstream f(folder/L"trace.jsonl");for(unsigned i=0;i<std::min(count.load(),512u);++i){auto& e=events[i];f<<"{\"index\":"<<i<<",\"call\":"<<e.call<<",\"kind\":"<<e.kind<<",\"tid\":"<<e.tid<<",\"qpc\":"<<e.qpc<<",\"pc\":"<<e.c.Rip<<",\"flags\":"<<(e.c.EFlags&~0x10100u)<<",\"mxcsr\":"<<e.c.MxCsr<<",\"registers\":[";for(int j=0;j<16;++j){if(j)f<<',';f<<reg(e.c,j,0);}f<<"],\"code\":\"";hex(f,e.code,e.code_size);f<<"\",\"xmm\":\"";hex(f,&e.c.Xmm0,256);f<<"\",\"output\":"<<e.output<<",\"output_ok\":"<<(e.value_ok?"true":"false")<<",\"value\":\"";hex(f,e.value,width);f<<"\",\"memory\":[";for(unsigned j=0;j<e.n;++j){auto& r=e.memory[j];if(j)f<<',';f<<"{\"address\":"<<r.address<<",\"size\":"<<r.size<<",\"ok\":"<<(r.ok?"true":"false")<<",\"bytes\":\"";hex(f,r.bytes,r.size);f<<"\"}";}f<<"]}\n";}
 f.close();std::ofstream m(folder/L"done.json");m<<"{\"calls\":"<<calls<<",\"events\":"<<std::min(count.load(),512u)<<",\"reason\":"<<reason<<",\"skipped_entries\":"<<skipped<<",\"armed\":"<<armed.size()<<",\"restored\":"<<restored<<",\"active_trap\":"<<(active?"true":"false")<<",\"elapsed_ms\":"<<double(now()-start)*1000/frequency<<",\"event_capacity_bytes\":"<<sizeof(events)<<",\"replacement_enabled\":false}";return 0;}
}
extern "C" __declspec(dllexport) DWORD WINAPI ArcInitialize(void* path){if(!path||entry||IsDebuggerPresent())return 1;folder=std::filesystem::path((wchar_t*)path).parent_path();std::ifstream f(folder/L"slice.txt");unsigned n;f>>entry>>exitPc>>target>>stride>>width>>limitCalls>>waitMs>>n;if(!f||!entry||exitPc<=entry||!n||n>16||!width||width>32||!limitCalls||limitCalls>4||!waitMs||waitMs>10000||!stride)return 2;for(unsigned i=0;i<n;++i){DWORD tid;f>>tid;threads.push_back(tid);}f>>stepCount;f>>destination.base>>destination.index>>destination.scale>>destination.disp>>destination.size;if(!stepCount||stepCount>65)return 3;for(unsigned i=0;i<stepCount;++i){auto& s=steps[i];f>>s.pc>>s.length>>s.n;if(!s.length||s.length>15||s.n>2)return 4;for(unsigned j=0;j<s.n;++j){auto& m=s.mem[j];f>>m.base>>m.index>>m.scale>>m.disp>>m.size;if(m.base < -1||m.base>16||m.index < -1||m.index>16||!m.size||m.size>32)return 5;}}if(!f)return 6;
 auto nt=GetModuleHandleW(L"ntdll.dll");getStatus=(GS)GetProcAddress(nt,"RtlGetLastNtStatus");setStatus=(SS)GetProcAddress(nt,"RtlSetLastWin32ErrorAndNtStatusFromNtStatus");if(!getStatus||!setStatus)return 7;HANDLE h=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(!h)return 8;CloseHandle(h);return 0;}
extern "C" __declspec(dllexport) DWORD WINAPI ArcStopOptimizer(void*){stop=true;return 0;}
BOOL WINAPI DllMain(HINSTANCE h,DWORD why,void*){if(why==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(h);return TRUE;}
