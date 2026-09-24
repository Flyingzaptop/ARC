// Focused native x64 diagnostic: hardware entry breakpoint + bounded TF trace.
// Never patches application code or substitutes computation. No DynamoRIO-wide translation.
#include <windows.h>
#include "arc/cpu/changed_words.hpp"
#include <tlhelp32.h>
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <vector>
#include <string>
#include <sstream>
namespace {
constexpr unsigned capacity=32768,training_capacity=20000000,tail_capacity_default=256;
static unsigned tail_capacity=tail_capacity_default;static bool consumer_mode{};
struct alignas(16) Event {CONTEXT context;LONGLONG qpc;uintptr_t teb;DWORD tid,kind;unsigned code_size;unsigned char code[16];DWORD last_error;LONG last_status;};
static Event* events{};
static unsigned char* encoded{};
static constexpr size_t encoded_capacity=6*1024*1024;
static constexpr unsigned long long disk_limit=2ull*1024*1024*1024;
static unsigned long long stream_bytes{};static bool io_failed{};
static std::uint64_t previous_words[sizeof(Event)/8]{};
static_assert(sizeof(Event)%8==0);
static std::atomic<unsigned> phase{},count{},in_handler{},collisions{},owner{};
static std::atomic<bool> abort_capture{},trap_pending{};
static bool training_mode{};static HANDLE stream_file=INVALID_HANDLE_VALUE;static std::atomic<unsigned> consumed{};static unsigned ring_capacity{};
static bool boundary_mode{};static std::atomic<bool> return_pending{};static uintptr_t stack_low{},stack_high{};
static unsigned event_limit=capacity;static double limit_ms=100;
static uintptr_t image_base{},entry{},entry_end{},return_pc{},return_sp{};static bool outermost{};static unsigned nested_skipped{};static unsigned char* pure_map{};static unsigned pure_image_size{};static unsigned long long pure_skipped{};static unsigned long long max_arg_span{};static std::atomic<unsigned> skipped_large_calls{};
static LONGLONG limit_ticks{};static LONGLONG frequency{},start_qpc{},exit_qpc{},handler_ticks{},last_handler_exit{};
static ULONG64 cpu_start{},cpu_finish{};
static unsigned post_steps{},completed_calls{};
struct SideEntry{CONTEXT context;DWORD tid;LONGLONG qpc;};static SideEntry side_entries[128];static std::atomic<unsigned> side_count{};
static unsigned char xstate_before[65536],xstate_after[65536];static unsigned xstate_before_bytes{},xstate_after_bytes{};static DWORD64 xstate_before_mask{},xstate_after_mask{};static bool xstate_before_ok{},xstate_after_ok{};
static bool save_xstate(CONTEXT* c,unsigned char* data,unsigned& bytes,DWORD64& mask){
 bytes=0;if(!GetXStateFeaturesMask(c,&mask))return false;
 for(unsigned id=2;id<64;++id)if(mask&(1ull<<id)){DWORD length{};void* value=LocateXStateFeature(c,id,&length);if(!value||length>65536-bytes-8)return false;memcpy(data+bytes,&id,4);memcpy(data+bytes+4,&length,4);memcpy(data+bytes+8,value,length);bytes+=8+length;}
 return true;
}
static constexpr size_t snapshot_limit=4*1024*1024;
static unsigned char *input_snapshot{},*output_snapshot{};
static uintptr_t snapshot_begin{};static size_t snapshot_bytes{};static bool snapshot_before{},snapshot_after{},stop_unsupported{};static unsigned semantic_stop{};static uintptr_t semantic_pc{};
static bool snapshot_private(uintptr_t begin,size_t bytes,void* output){
 if(!begin||!bytes||bytes>snapshot_limit)return false;
 auto end=begin+bytes;if(end<begin)return false;
 for(auto p=begin;p<end;){MEMORY_BASIC_INFORMATION m{};if(VirtualQuery(reinterpret_cast<void*>(p),&m,sizeof(m))!=sizeof(m)||m.State!=MEM_COMMIT||m.Type!=MEM_PRIVATE||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;auto next=uintptr_t(m.BaseAddress)+m.RegionSize;if(next<=p)return false;p=next;}
 SIZE_T got{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(begin),output,bytes,&got)&&got==bytes;
}
static std::atomic<unsigned> reason{}; // 1 events,2 time,3 exception,4 wait timeout,5 no entry,6 decode/read
static std::filesystem::path destination;
static void* veh{};
static std::vector<DWORD> armed;
static HANDLE self{};
using GetStatusFn=LONG(NTAPI*)();using SetStatusFn=void(NTAPI*)(LONG);static GetStatusFn get_status{};static SetStatusFn set_status{};
struct ThreadStatusGuard{DWORD error=GetLastError();LONG status=get_status?get_status():0;~ThreadStatusGuard(){if(set_status)set_status(status);SetLastError(error);}};
static ULONG64 thread_cpu(){FILETIME a,b,k,u;if(!GetThreadTimes(GetCurrentThread(),&a,&b,&k,&u))return 0;return (ULONG64(k.dwHighDateTime)<<32)+k.dwLowDateTime+(ULONG64(u.dwHighDateTime)<<32)+u.dwLowDateTime;}
static LONGLONG now(){LARGE_INTEGER n;QueryPerformanceCounter(&n);return n.QuadPart;}
static void disable_ours(CONTEXT* c){if(c->Dr0==entry){c->Dr0=0;c->Dr7&=~DWORD64(0x30003);}c->Dr6=0;}
static void record(CONTEXT* c,unsigned kind,const ThreadStatusGuard& status){
 if(training_mode&&(kind==0||kind==3)&&pure_map&&c->Rip>=image_base&&c->Rip-image_base<pure_image_size){auto rva=c->Rip-image_base;if(pure_map[rva/8]&(1u<<(rva%8))){++pure_skipped;return;}}
 unsigned i=count.load(std::memory_order_relaxed);if(i>=event_limit){reason=1;abort_capture=true;return;}
 while(training_mode&&i-consumed.load(std::memory_order_acquire)>=ring_capacity){
  if(abort_capture||now()-start_qpc>limit_ticks){reason=2;abort_capture=true;return;}Sleep(1);
 }
 auto& e=events[i%ring_capacity];e.context=*c;e.context.EFlags&=~0x100u;e.qpc=now();e.tid=GetCurrentThreadId();e.teb=uintptr_t(NtCurrentTeb());e.kind=kind;e.last_error=status.error;e.last_status=status.status;
 SIZE_T read{};ReadProcessMemory(self,reinterpret_cast<void*>(c->Rip),e.code,15,&read);e.code_size=unsigned(read);
 if(training_mode&&stop_unsupported&&!completed_calls&&kind!=2&&read>=2){
  if(e.code[0]==0xf0){semantic_stop=1;semantic_pc=c->Rip;}
  else if(e.code[0]==0x0f&&e.code[1]==0x05){semantic_stop=2;semantic_pc=c->Rip;}
 }
 count.store(i+1,std::memory_order_release);
}
LONG CALLBACK exception(EXCEPTION_POINTERS* p){
 ThreadStatusGuard thread_status;auto* c=p->ContextRecord;auto code=p->ExceptionRecord->ExceptionCode;DWORD tid=GetCurrentThreadId();
 if(code!=EXCEPTION_SINGLE_STEP){if(trap_pending&&tid==owner){abort_capture=true;reason=3;c->EFlags&=~0x100u;trap_pending=false;phase=3;}return EXCEPTION_CONTINUE_SEARCH;}
 bool own_break=(c->Dr6&1)&&c->Dr0==entry&&c->Rip==entry;
 bool own_return=(c->Dr6&2)&&c->Dr1==return_pc&&return_pending&&tid==owner;
 if(!own_break&&!own_return&&!(trap_pending&&tid==owner))return EXCEPTION_CONTINUE_SEARCH;
 in_handler.fetch_add(1);auto entered=now();
 if(abort_capture){
  disable_ours(c);if(tid==owner){c->EFlags&=~0x100u;trap_pending=false;if(c->Dr1==return_pc){c->Dr1=0;c->Dr7&=~DWORD64(0xf0000c);}return_pending=false;}
  c->EFlags|=0x10000u;phase=3;in_handler.fetch_sub(1);return EXCEPTION_CONTINUE_EXECUTION;
 }
 if(own_return){
  if(c->Rip!=return_pc||c->Rsp!=return_sp){c->EFlags|=0x10000u;in_handler.fetch_sub(1);return EXCEPTION_CONTINUE_EXECUTION;}
  if(!abort_capture){completed_calls=1;exit_qpc=entered;cpu_finish=thread_cpu();xstate_after_ok=save_xstate(c,xstate_after,xstate_after_bytes,xstate_after_mask);if(snapshot_before)snapshot_after=snapshot_private(snapshot_begin,snapshot_bytes,output_snapshot);record(c,2,thread_status);}
  c->Dr1=0;c->Dr7&=~DWORD64(0xf0000c);c->Dr6=0;c->EFlags|=0x10000u;return_pending=false;if(consumer_mode&&!abort_capture){c->EFlags|=0x100u;trap_pending=true;phase=2;}else phase=3;handler_ticks+=now()-entered;in_handler.fetch_sub(1);return EXCEPTION_CONTINUE_EXECUTION;
 }
 if(own_break){
  if(phase==1&&outermost){uintptr_t ret{};SIZE_T got{};if(ReadProcessMemory(self,reinterpret_cast<void*>(c->Rsp),&ret,8,&got)&&got==8&&ret>=entry&&ret<entry_end){++nested_skipped;c->EFlags|=0x10000u;c->Dr6=0;in_handler.fetch_sub(1);return EXCEPTION_CONTINUE_EXECUTION;}}
  // Generic observational size hypothesis, never a replacement guard: two
  // leading pointer arguments delimiting a small private-memory interval.
  if(phase==1&&max_arg_span){
   MEMORY_BASIC_INFORMATION info{};bool fits=c->Rcx>=65536&&c->Rdx>c->Rcx&&c->Rdx-c->Rcx<=max_arg_span;
   if(fits)fits=VirtualQuery(reinterpret_cast<void*>(c->Rcx),&info,sizeof(info))==sizeof(info)&&info.State==MEM_COMMIT&&info.Type==MEM_PRIVATE&&!(info.Protect&(PAGE_NOACCESS|PAGE_GUARD))&&c->Rdx<=uintptr_t(info.BaseAddress)+info.RegionSize;
   if(!fits){++skipped_large_calls;c->EFlags|=0x10000u;c->Dr6=0;in_handler.fetch_sub(1);return EXCEPTION_CONTINUE_EXECUTION;}
  }
  disable_ours(c);unsigned expected=1;
  if(!phase.compare_exchange_strong(expected,6)){++collisions;auto slot=side_count.fetch_add(1);if(slot<128){side_entries[slot].context=*c;side_entries[slot].tid=tid;side_entries[slot].qpc=entered;}c->EFlags|=0x10000u;in_handler.fetch_sub(1);return EXCEPTION_CONTINUE_EXECUTION;}
  owner=tid;xstate_before_ok=save_xstate(c,xstate_before,xstate_before_bytes,xstate_before_mask);start_qpc=entered;cpu_start=thread_cpu();return_sp=c->Rsp+8;auto* tib=reinterpret_cast<NT_TIB*>(NtCurrentTeb());stack_low=uintptr_t(tib->StackLimit);stack_high=uintptr_t(tib->StackBase);SIZE_T bytes{};
  if(!ReadProcessMemory(self,reinterpret_cast<void*>(c->Rsp),&return_pc,8,&bytes)||bytes!=8){abort_capture=true;reason=6;}
  if(training_mode&&input_snapshot&&c->Rcx>=65536&&c->Rdx>c->Rcx&&c->Rdx-c->Rcx<=snapshot_limit&&!(c->Rcx>=stack_low&&c->Rcx<stack_high)){
   snapshot_begin=c->Rcx;snapshot_bytes=c->Rdx-c->Rcx;snapshot_before=snapshot_private(snapshot_begin,snapshot_bytes,input_snapshot);
  }
  if(boundary_mode&&!abort_capture){
   record(c,1,thread_status);c->Dr1=return_pc;c->Dr7=(c->Dr7&~DWORD64(0xf0000c))|4;c->Dr6=0;c->EFlags|=0x10000u;return_pending=true;phase=2;handler_ticks+=now()-entered;in_handler.fetch_sub(1);return EXCEPTION_CONTINUE_EXECUTION;
  }
  trap_pending=true;record(c,1,thread_status);phase=2; // pre-entry architectural state
 }else{
  if(c->Rip==return_pc&&c->Rsp==return_sp&&!completed_calls){completed_calls=1;exit_qpc=entered;cpu_finish=thread_cpu();xstate_after_ok=save_xstate(c,xstate_after,xstate_after_bytes,xstate_after_mask);if(snapshot_before)snapshot_after=snapshot_private(snapshot_begin,snapshot_bytes,output_snapshot);record(c,2,thread_status);}
  else record(c,completed_calls?3:0,thread_status);
  if(completed_calls&&++post_steps>=tail_capacity){abort_capture=true;}
 }
 if(semantic_stop&&!completed_calls&&!abort_capture){
  c->EFlags&=~0x100u;trap_pending=false;c->Dr1=return_pc;c->Dr7=(c->Dr7&~DWORD64(0xf0000c))|4;c->Dr6=0;return_pending=true;phase=2;handler_ticks+=now()-entered;in_handler.fetch_sub(1);return EXCEPTION_CONTINUE_EXECUTION;
 }
 if(now()-start_qpc>limit_ticks){reason=2;abort_capture=true;}
 if(abort_capture){c->EFlags&=~0x100u;trap_pending=false;phase=3;}else c->EFlags|=0x100u;
 c->Dr6=0;last_handler_exit=now();handler_ticks+=last_handler_exit-entered;in_handler.fetch_sub(1);return EXCEPTION_CONTINUE_EXECUTION;
}
struct Suspension{HANDLE h;bool owns;Suspension(HANDLE x):h(x),owns(SuspendThread(x)!=DWORD(-1)){}~Suspension(){if(owns)ResumeThread(h);}};
static bool debug_thread(DWORD tid,bool enable){
 HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,tid);if(!h)return false;bool ok=false;
 {Suspension stop(h);if(stop.owns){CONTEXT c{};c.ContextFlags=CONTEXT_DEBUG_REGISTERS|CONTEXT_CONTROL;if(GetThreadContext(h,&c)){
  if(enable){if(!(c.Dr7&0xff)&&!(c.EFlags&0x100)&&!c.Dr0&&!c.Dr1){c.Dr0=entry;c.Dr7=(c.Dr7&~DWORD64(0xf0003))|1;ok=SetThreadContext(h,&c)!=FALSE;}}
  else {if(c.Dr0==entry)disable_ours(&c);if(tid==owner&&c.Dr1==return_pc){c.Dr1=0;c.Dr7&=~DWORD64(0xf0000c);return_pending=false;}ok=SetThreadContext(h,&c)!=FALSE;}
 }}}CloseHandle(h);return ok;
}
static bool drain(){
 if(!training_mode)return true;
 if(io_failed)return false;
 unsigned lo=consumed.load(),hi=count.load(std::memory_order_acquire);
 while(lo<hi){unsigned batch=std::min<unsigned>({hi-lo,ring_capacity-lo%ring_capacity,4096});DWORD written{};
  size_t used=0;for(unsigned i=0;i<batch;++i){auto bytes=arc::cpu::encode_changed_words(events+(lo+i)%ring_capacity,previous_words,sizeof(Event)/8,encoded+used,encoded_capacity-used);if(!bytes){reason=7;abort_capture=true;io_failed=true;return false;}used+=bytes;}
  if(stream_bytes+used>disk_limit){reason=9;abort_capture=true;io_failed=true;return false;}
  if(!WriteFile(stream_file,encoded,DWORD(used),&written,nullptr)||written!=used){reason=7;abort_capture=true;io_failed=true;return false;}stream_bytes+=used;
  lo+=batch;consumed.store(lo,std::memory_order_release);
 }
 return true;
}
static void dump(){
 std::filesystem::create_directories(destination);
 unsigned n=count.load(std::memory_order_acquire);
 if(!training_mode){std::ofstream out(destination/L"events.jsonl");
 for(unsigned i=0;i<n;++i){auto& e=events[i];auto& c=e.context;
  out<<"{\"index\":"<<i<<",\"kind\":"<<e.kind<<",\"tid\":"<<e.tid<<",\"qpc\":"<<e.qpc<<",\"teb\":"<<e.teb<<",\"last_error\":"<<e.last_error<<",\"last_status\":"<<e.last_status<<",\"rip\":"<<c.Rip<<",\"eflags\":"<<c.EFlags<<",\"mxcsr\":"<<c.MxCsr<<",\"code\":\"";
  for(unsigned k=0;k<e.code_size;++k)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(e.code[k]);out<<std::dec<<"\",\"registers\":[";
  DWORD64 regs[]={c.Rax,c.Rcx,c.Rdx,c.Rbx,c.Rsp,c.Rbp,c.Rsi,c.Rdi,c.R8,c.R9,c.R10,c.R11,c.R12,c.R13,c.R14,c.R15};
  for(unsigned k=0;k<16;++k){if(k)out<<',';out<<regs[k];}out<<"]}\n";
 }
 std::ofstream raw(destination/L"contexts.bin",std::ios::binary);for(unsigned i=0;i<n;++i)raw.write(reinterpret_cast<char*>(&events[i].context),sizeof(CONTEXT));
 }
 if(snapshot_before){std::ofstream f(destination/L"input-span.bin",std::ios::binary);f.write(reinterpret_cast<char*>(input_snapshot),snapshot_bytes);}
 if(snapshot_after){std::ofstream f(destination/L"output-span.bin",std::ios::binary);f.write(reinterpret_cast<char*>(output_snapshot),snapshot_bytes);}
 if(xstate_before_ok){std::ofstream f(destination/L"xstate-before.bin",std::ios::binary);f.write(reinterpret_cast<char*>(xstate_before),xstate_before_bytes);}
 if(xstate_after_ok){std::ofstream f(destination/L"xstate-after.bin",std::ios::binary);f.write(reinterpret_cast<char*>(xstate_after),xstate_after_bytes);}
 {std::ofstream f(destination/L"concurrent-entries.json");f<<"[";for(unsigned i=0;i<std::min(side_count.load(),128u);++i){auto& e=side_entries[i];if(i)f<<",";f<<"{\"tid\":"<<e.tid<<",\"qpc\":"<<e.qpc<<",\"rcx\":"<<e.context.Rcx<<",\"rdx\":"<<e.context.Rdx<<",\"r8\":"<<e.context.R8<<",\"rsp\":"<<e.context.Rsp<<"}";}f<<"]";}
 std::ofstream meta(destination/L"capture.json");meta<<"{\"schema\":1,\"events\":"<<n<<",\"capacity\":"<<event_limit<<",\"context_bytes\":"<<sizeof(CONTEXT)<<",\"allocated_bytes\":"<<(sizeof(Event)*ring_capacity+(training_mode?encoded_capacity:0)+(pure_map?(pure_image_size+7)/8:0)+(input_snapshot?2*snapshot_limit:0))<<",\"streaming\":"<<(training_mode?"true":"false")<<",\"stream_encoding\":\"changed_words_v1\",\"stream_bytes\":"<<stream_bytes<<",\"disk_limit\":"<<disk_limit<<",\"written_events\":"<<consumed<<",\"event_bytes\":"<<sizeof(Event)<<",\"qpc_offset\":"<<offsetof(Event,qpc)<<",\"teb_offset\":"<<offsetof(Event,teb)<<",\"tid_offset\":"<<offsetof(Event,tid)<<",\"kind_offset\":"<<offsetof(Event,kind)<<",\"code_size_offset\":"<<offsetof(Event,code_size)<<",\"code_offset\":"<<offsetof(Event,code)<<",\"last_error_offset\":"<<offsetof(Event,last_error)<<",\"last_status_offset\":"<<offsetof(Event,last_status)<<",\"thread_status_preserved\":"<<(get_status&&set_status?"true":"false")<<",\"rax_offset\":"<<offsetof(CONTEXT,Rax)<<",\"rip_offset\":"<<offsetof(CONTEXT,Rip)<<",\"eflags_offset\":"<<offsetof(CONTEXT,EFlags)<<",\"mxcsr_offset\":"<<offsetof(CONTEXT,MxCsr)<<",\"xmm_offset\":"<<offsetof(CONTEXT,Xmm0)<<",\"main_base\":"<<uintptr_t(GetModuleHandleW(nullptr))<<",\"entry\":"<<entry<<",\"max_arg_span_hypothesis\":"<<max_arg_span<<",\"skipped_size_hypothesis_calls\":"<<skipped_large_calls<<",\"semantic_stop\":"<<semantic_stop<<",\"semantic_stop_pc\":"<<semantic_pc<<",\"snapshot_begin\":"<<snapshot_begin<<",\"snapshot_bytes\":"<<snapshot_bytes<<",\"snapshot_before\":"<<(snapshot_before?"true":"false")<<",\"snapshot_after\":"<<(snapshot_after?"true":"false")<<",\"consumer_mode\":"<<(consumer_mode?"true":"false")<<",\"tail_instruction_limit\":"<<tail_capacity<<",\"xstate_before_ok\":"<<(xstate_before_ok?"true":"false")<<",\"xstate_after_ok\":"<<(xstate_after_ok?"true":"false")<<",\"xstate_before_mask\":"<<xstate_before_mask<<",\"xstate_after_mask\":"<<xstate_after_mask<<",\"boundary_mode\":"<<(boundary_mode?"true":"false")<<",\"stack_low\":"<<stack_low<<",\"stack_high\":"<<stack_high<<",\"entry_end\":"<<entry_end<<",\"outermost_requested\":"<<(outermost?"true":"false")<<",\"nested_entries_skipped\":"<<nested_skipped<<",\"pure_instructions_skipped\":"<<pure_skipped<<",\"return_pc\":"<<return_pc<<",\"return_sp\":"<<return_sp<<",\"owner_tid\":"<<owner<<",\"armed_threads\":"<<armed.size()<<",\"concurrent_entry_hits\":"<<collisions<<",\"completed_calls\":"<<completed_calls<<",\"reason\":"<<reason<<",\"qpc_frequency\":"<<frequency<<",\"start_qpc\":"<<start_qpc<<",\"exit_qpc\":"<<exit_qpc<<",\"handler_ticks\":"<<handler_ticks<<",\"instrumented_thread_cpu_100ns\":"<<(completed_calls&&cpu_finish>=cpu_start?std::to_string(cpu_finish-cpu_start):std::string("null"))<<",\"max_trace_ms\":"<<limit_ms<<",\"trap_cleanup_pending\":"<<(trap_pending?"true":"false")<<",\"other_thread_memory_observed\":false,\"kernel_memory_effects_observed\":false,\"extended_xstate_complete\":false,\"replacement_enabled\":false}";
}
DWORD WINAPI worker(void*){
 LARGE_INTEGER f;QueryPerformanceFrequency(&f);frequency=f.QuadPart;limit_ticks=LONGLONG(limit_ms*double(frequency)/1000.);self=GetCurrentProcess();
 ring_capacity=std::min(event_limit,capacity);armed.reserve(128);
 if(training_mode){input_snapshot=static_cast<unsigned char*>(VirtualAlloc(nullptr,snapshot_limit*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!input_snapshot){phase=4;return 7;}output_snapshot=input_snapshot+snapshot_limit;encoded=static_cast<unsigned char*>(VirtualAlloc(nullptr,encoded_capacity,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!encoded){phase=4;return 7;}stream_file=CreateFileW((destination/L"events.bin").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);if(stream_file==INVALID_HANDLE_VALUE){phase=4;return 8;}}
 events=static_cast<Event*>(VirtualAlloc(nullptr,sizeof(Event)*ring_capacity,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!events){phase=4;return 2;}
 veh=AddVectoredExceptionHandler(1,exception);if(!veh){phase=4;return 3;}phase=1;
 HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 t{};t.dwSize=sizeof(t);
 if(Thread32First(snap,&t))do{if(t.th32OwnerProcessID==GetCurrentProcessId()&&t.th32ThreadID!=GetCurrentThreadId()&&armed.size()<128&&debug_thread(t.th32ThreadID,true))armed.push_back(t.th32ThreadID);}while(Thread32Next(snap,&t));CloseHandle(snap);
 auto began=now();while((phase==1||phase==6)&&double(now()-began)/frequency<2.){if(!drain())break;Sleep(1);}
 if(phase==1||phase==6){reason=5;abort_capture=true;phase=3;}
 while(phase==2){if(!drain()||abort_capture)break;if(now()-start_qpc>limit_ticks+frequency/20){reason=4;abort_capture=true;break;}Sleep(1);}
 for(DWORD tid:armed)debug_thread(tid,false);
 // An owner blocked in the kernel observes cancellation on its next user step.
 // Keep DLL and VEH alive; never unload a handler while such a trap is pending.
 abort_capture=true;while(in_handler.load()){drain();Sleep(1);}
 drain();if(stream_file!=INVALID_HANDLE_VALUE){CloseHandle(stream_file);stream_file=INVALID_HANDLE_VALUE;}
 dump();phase=4;return 0;
}
}
extern "C" __declspec(dllexport) DWORD WINAPI ArcInitialize(void* path){
 if(!path||phase.load())return 1;try{auto ntdll=GetModuleHandleW(L"ntdll.dll");get_status=reinterpret_cast<GetStatusFn>(GetProcAddress(ntdll,"RtlGetLastNtStatus"));set_status=reinterpret_cast<SetStatusFn>(GetProcAddress(ntdll,"RtlSetLastWin32ErrorAndNtStatusFromNtStatus"));if(!get_status||!set_status)return 9;
  destination=std::filesystem::path(static_cast<const wchar_t*>(path)).parent_path();std::ifstream config(destination/L"request.txt");uint64_t rva{},length{};unsigned timestamp{},image_size{};config>>std::hex>>rva>>length>>timestamp>>image_size>>std::dec>>event_limit>>limit_ms;std::string mode;config>>mode;consumer_mode=mode=="consumer";boundary_mode=mode=="boundary"||consumer_mode;training_mode=mode=="training"||consumer_mode;tail_capacity=consumer_mode?65536:tail_capacity_default;if(config.eof())config.clear();config>>max_arg_span;if(config.eof())config.clear();config>>outermost;if(config.eof())config.clear();config>>stop_unsupported;if(config.eof())config.clear();
  if(!config||!std::isfinite(limit_ms)||length==0||length>65536||event_limit<32||event_limit>(training_mode?training_capacity:capacity)||limit_ms<1||limit_ms>(training_mode?60000:250))return 2;
  auto base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
  if(nt->FileHeader.TimeDateStamp!=timestamp||nt->OptionalHeader.SizeOfImage!=image_size||rva+length>image_size)return 3;
  std::vector<char> expected(length),actual(length);std::ifstream bytes(destination/L"expected-code.bin",std::ios::binary);bytes.read(expected.data(),length);SIZE_T got{};
  if(!bytes||!ReadProcessMemory(GetCurrentProcess(),base+rva,actual.data(),length,&got)||got!=length||memcmp(expected.data(),actual.data(),length))return 4;
  if(IsDebuggerPresent())return 5;image_base=uintptr_t(base);entry=image_base+rva;entry_end=entry+length;
  // A skip map is accepted only after byte-verifying every decoded function.
  std::ifstream functions(destination/L"checked-functions.bin",std::ios::binary);
  if(training_mode&&functions&&image_size<=64*1024*1024){
   unsigned count{};functions.read(reinterpret_cast<char*>(&count),4);if(!functions||count>128)return 8;
   for(unsigned i=0;i<count;++i){unsigned address{},bytes{};functions.read(reinterpret_cast<char*>(&address),4);functions.read(reinterpret_cast<char*>(&bytes),4);if(!functions||!bytes||bytes>65536||uint64_t(address)+bytes>image_size)return 8;
    std::vector<char> wanted(bytes),present(bytes);functions.read(wanted.data(),bytes);SIZE_T copied{};if(!functions||!ReadProcessMemory(self=GetCurrentProcess(),base+address,present.data(),bytes,&copied)||copied!=bytes||wanted!=present)return 8;
   }
   pure_image_size=image_size;pure_map=static_cast<unsigned char*>(VirtualAlloc(nullptr,(image_size+7)/8,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!pure_map)return 8;
   std::ifstream map(destination/L"pure-rvas.bin",std::ios::binary);unsigned address{};while(map.read(reinterpret_cast<char*>(&address),4)){if(address>=image_size)return 8;pure_map[address/8]|=1u<<(address%8);}
  }
  HANDLE h=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(!h)return 6;CloseHandle(h);return 0;
 }catch(...){return 7;}
}
extern "C" __declspec(dllexport) DWORD WINAPI ArcStopOptimizer(void*){reason=8;abort_capture=true;return 0;}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void*){if(reason==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(module);return TRUE;}
