#include <windows.h>
#include <tlhelp32.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
#include <algorithm>
#include <string>
#include <stdexcept>
#include <iomanip>

namespace {
struct Handle {HANDLE value{};explicit Handle(HANDLE h=nullptr):value(h){}~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;Handle(Handle&& other)noexcept:value(other.value){other.value=nullptr;}Handle& operator=(Handle&& other)noexcept{if(this!=&other){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=other.value;other.value=nullptr;}return *this;}};
UINT64 ticks(FILETIME t){return (UINT64(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
struct Times {UINT64 user{},kernel{},cycles{};bool valid{};};
Times thread_times(HANDLE thread){FILETIME created{},exited{},kernel{},user{};Times t;t.valid=GetThreadTimes(thread,&created,&exited,&kernel,&user)!=FALSE;if(t.valid){t.user=ticks(user);t.kernel=ticks(kernel);QueryThreadCycleTime(thread,&t.cycles);}return t;}
UINT64 process_ticks(HANDLE process){FILETIME created{},exited{},kernel{},user{};if(!GetProcessTimes(process,&created,&exited,&kernel,&user))throw std::runtime_error("GetProcessTimes failed");return ticks(user)+ticks(kernel);}
std::string utf8(const wchar_t* value){if(!value)return {};const int count=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);if(count<=1)return {};std::string out(count,'\0');WideCharToMultiByte(CP_UTF8,0,value,-1,out.data(),count,nullptr,nullptr);out.pop_back();return out;}
void quoted(std::ostream& out,const std::string& value){constexpr char hex[]="0123456789abcdef";out<<'"';for(unsigned char c:value){if(c=='"'||c=='\\')out<<'\\'<<c;else if(c<32)out<<"\\u00"<<hex[c>>4]<<hex[c&15];else out<<c;}out<<'"';}
struct Thread {DWORD id;Handle handle;std::string name;Times before,after;};
}
int wmain(int argc,wchar_t** argv)try{
    if(argc!=4){std::cerr<<"Usage: arc-process-sample <authorized PID> <seconds 1..60> <new JSON>\n";return 2;}
    wchar_t* end=nullptr;const DWORD pid=wcstoul(argv[1],&end,10);if(!pid||!end||*end)throw std::runtime_error("Invalid PID");
    const auto seconds=wcstoul(argv[2],&end,10);if(!seconds||seconds>60||!end||*end)throw std::runtime_error("Duration must be 1..60 seconds");
    const auto path=std::filesystem::absolute(argv[3]);if(std::filesystem::exists(path)||!std::filesystem::is_directory(path.parent_path()))throw std::runtime_error("New output in an existing directory required");
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));if(!process.value)throw std::runtime_error("Cannot query authorized process");
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0));if(snapshot.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Thread inventory failed");
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);std::vector<Thread> threads;unsigned inaccessible=0;bool capped=false;
    if(Thread32First(snapshot.value,&entry))do{if(entry.th32OwnerProcessID!=pid)continue;if(threads.size()>=4096){capped=true;break;}
        Handle handle(OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID));if(!handle.value){++inaccessible;continue;}
        PWSTR name=nullptr;std::string description;if(SUCCEEDED(GetThreadDescription(handle.value,&name))){description=utf8(name);LocalFree(name);}
        threads.push_back({entry.th32ThreadID,std::move(handle),description,{},{}});
    }while(Thread32Next(snapshot.value,&entry));
    const auto start=std::chrono::steady_clock::now();const auto before_process=process_ticks(process.value);
    for(auto& thread:threads)thread.before=thread_times(thread.handle.value);
    const auto wait=WaitForSingleObject(process.value,static_cast<DWORD>(seconds)*1000);
    if(wait!=WAIT_TIMEOUT&&wait!=WAIT_OBJECT_0)throw std::runtime_error("Process wait failed");
    double observed_ms=0;
    for(auto& thread:threads){thread.after=thread_times(thread.handle.value);if(thread.before.valid&&thread.after.valid)observed_ms+=double(thread.after.user-thread.before.user+thread.after.kernel-thread.before.kernel)/10000.;}
    const auto after_process=process_ticks(process.value);const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::sort(threads.begin(),threads.end(),[](const Thread& a,const Thread& b){return a.after.user+a.after.kernel-a.before.user-a.before.kernel>b.after.user+b.after.kernel-b.before.user-b.before.kernel;});
    const auto temporary=std::filesystem::path(path.wstring()+L".tmp");std::ofstream out(temporary);out<<std::setprecision(12)<<"{\"schema\":1,\"pid\":"<<pid<<",\"seconds\":"<<elapsed<<",\"process_exited\":"<<(wait==WAIT_OBJECT_0?"true":"false")
        <<",\"thread_suspension\":false,\"memory_readback\":false,\"new_threads_not_sampled\":true,\"inventory_capped\":"<<(capped?"true":"false")<<",\"inaccessible_threads\":"<<inaccessible
        <<",\"process_cpu_ms\":"<<double(after_process-before_process)/10000.<<",\"observed_thread_cpu_ms\":"<<observed_ms<<",\"threads\":[";
    bool first=true;for(const auto& t:threads){if(!t.before.valid||!t.after.valid)continue;if(!first)out<<',';first=false;const double user=double(t.after.user-t.before.user)/10000.,kernel=double(t.after.kernel-t.before.kernel)/10000.;
        out<<"{\"id\":"<<t.id<<",\"name\":";quoted(out,t.name);out<<",\"user_ms\":"<<user<<",\"kernel_ms\":"<<kernel<<",\"percent_of_one_logical_cpu\":"<<(user+kernel)/(elapsed*10.)<<",\"cycles\":"<<(t.after.cycles>=t.before.cycles?t.after.cycles-t.before.cycles:0)<<'}';}
    out<<"]}";out.close();if(!out)throw std::runtime_error("Output write failed");std::filesystem::rename(temporary,path);
    std::cout<<"Read-only CPU thread sample complete: "<<threads.size()<<" threads, "<<elapsed<<" seconds; no injection or suspension\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
