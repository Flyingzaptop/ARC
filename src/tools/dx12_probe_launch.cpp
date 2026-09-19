#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <stdexcept>

namespace {
struct Handle{HANDLE h{};~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete;explicit Handle(HANDLE p=nullptr):h(p){};};
void require(bool value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+std::to_string(GetLastError()));}
std::wstring quote(const std::wstring& s){
    std::wstring out=L"\"";std::size_t slashes=0;
    for(wchar_t c:s){if(c==L'\\'){++slashes;continue;}if(c==L'\"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}
    out.append(slashes*2,L'\\');out+=L'\"';return out;
}
std::uintptr_t remote_module(DWORD pid,const std::wstring& name){
    HANDLE raw=INVALID_HANDLE_VALUE;
    for(int retry=0;retry<20;++retry){raw=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);if(raw!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_BAD_LENGTH)break;Sleep(10);}
    Handle snapshot(raw);
    MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(snapshot.h!=INVALID_HANDLE_VALUE&&Module32FirstW(snapshot.h,&entry))do{if(_wcsicmp(entry.szModule,name.c_str())==0)return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);}while(Module32NextW(snapshot.h,&entry));
    return 0;
}
DWORD remote_call(HANDLE process,void* function,const std::wstring& argument){
    const auto bytes=(argument.size()+1)*sizeof(wchar_t);void* remote=VirtualAllocEx(process,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);require(remote!=nullptr,"allocate argument");
    SIZE_T written{};
    if(!WriteProcessMemory(process,remote,argument.c_str(),bytes,&written)||written!=bytes){VirtualFreeEx(process,remote,0,MEM_RELEASE);throw std::runtime_error("write argument");}
    Handle thread(CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(function),remote,0,nullptr));
    if(!thread.h){VirtualFreeEx(process,remote,0,MEM_RELEASE);throw std::runtime_error("start probe initialization");}
    // On timeout leave the argument alive until process exit: the thread may still use it.
    require(WaitForSingleObject(thread.h,30000)==WAIT_OBJECT_0,"probe initialization timeout");DWORD code{};require(GetExitCodeThread(thread.h,&code)!=FALSE,"thread result");VirtualFreeEx(process,remote,0,MEM_RELEASE);return code;
}
}
int wmain(int argc,wchar_t** argv)try{
    const bool vrs_on=argc>1&&std::wstring(argv[1])==L"--vrs-2x2";
    const bool vrs_off=argc>1&&std::wstring(argv[1])==L"--vrs-off";
    const bool passive=argc>1&&std::wstring(argv[1])==L"--passive";
    const bool lean=argc>1&&std::wstring(argv[1])==L"--lean";
    const bool profile_stop=argc>1&&std::wstring(argv[1])==L"--gpu-profile-stop";
    const bool profile=argc>1&&std::wstring(argv[1])==L"--gpu-profile";
    const bool experiment=passive||lean||vrs_on||vrs_off||profile_stop;
    const bool features=argc>1&&std::wstring(argv[1])==L"--features";
    const bool image=argc>1&&std::wstring(argv[1])==L"--image";
    const bool timing=argc>1&&std::wstring(argv[1])==L"--measure";
    const bool capture=experiment||profile||features||image||timing||(argc>1&&std::wstring(argv[1])==L"--capture");
    const bool attach=capture||(argc>1&&std::wstring(argv[1])==L"--attach");
    if(argc<(experiment?4:attach?5:4)){std::wcerr<<L"Usage: arc-dx12-probe-launch <owned executable> <probe DLL> <output JSON> [application args...]\nOr: --attach <explicit authorized PID> <probe DLL> <output JSON>\n";return 2;}
    const auto dll=std::filesystem::absolute(argv[attach?3:2]);
    const auto output=experiment?std::filesystem::path{}:std::filesystem::absolute(argv[attach?4:3]);
    require(std::filesystem::is_regular_file(dll)&&(experiment||!std::filesystem::exists(output)),"input DLL/new output path");
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION info{};
    if(attach){
        wchar_t* end=nullptr;const auto pid=wcstoul(argv[2],&end,10);require(end&&!*end&&pid&&pid!=GetCurrentProcessId(),"explicit target PID");info.dwProcessId=pid;
        info.hProcess=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid);require(info.hProcess!=nullptr,"open authorized target");
    }else{
        const auto exe=std::filesystem::absolute(argv[1]);require(std::filesystem::is_regular_file(exe),"input executable");
        std::wstring command=quote(exe.wstring());for(int i=4;i<argc;++i)command+=L" "+quote(argv[i]);
        require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup,&info)!=FALSE,"launch owned process");
    }
    Handle process(info.hProcess),main_thread(info.hThread);
    std::wcout<<(attach?L"attached_pid=":L"launched_pid=")<<info.dwProcessId<<std::endl;
    BOOL wow{};require(IsWow64Process(process.h,&wow)&&!wow,"x64 target required");
    const auto load=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");require(load!=nullptr,"LoadLibraryW");
    HMODULE owner{};require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(load),&owner)!=FALSE,"loader module");
    wchar_t owner_path[32768]{};require(GetModuleFileNameW(owner,owner_path,32768)>0,"loader module path");
    std::uintptr_t base=0;for(int i=0;i<100&&!base;++i){base=remote_module(info.dwProcessId,std::filesystem::path(owner_path).filename().wstring());if(!base)Sleep(20);}
    require(base!=0,"target loader module unavailable");
    if(capture){
        const auto remote_base=remote_module(info.dwProcessId,dll.filename().wstring());require(remote_base!=0,"Target must already have the observer attached");
        HMODULE local=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);require(local!=nullptr,"read capture export");
        auto request=GetProcAddress(local,profile?"ArcRequestGpuProfile":profile_stop?"ArcStopGpuProfile":features?"ArcRequestFeatures":passive?"ArcUsePassiveMode":lean?"ArcUseLeanMode":experiment?"ArcExperimentalVrs":timing?"ArcRequestTiming":image?"ArcRequestImage":"ArcRequestFrame");const auto offset=reinterpret_cast<std::uintptr_t>(request)-reinterpret_cast<std::uintptr_t>(local);FreeLibrary(local);require(request!=nullptr,"capture export");
        const auto argument=experiment?std::wstring(vrs_on?L"2x2":L"off"):(timing||profile)&&argc>5?std::wstring(argv[5])+L"|"+output.wstring():output.wstring();
        const auto code=remote_call(process.h,reinterpret_cast<void*>(remote_base+offset),argument);
        if(code!=0)throw std::runtime_error("Capture request refused, ARC status "+std::to_string(code));std::wcout<<(profile?L"Bounded GPU cost profile":profile_stop?L"GPU profile stopped":features?L"GPU tile features":experiment?(passive?L"Passive observation and cached-list rollback retained":lean?L"Detailed observation disabled":vrs_on?L"Experimental VRS enabled":L"Original command execution restored"):timing?L"Bounded Present cadence":image?L"Image readback":L"Frame graph")<<L" requested for PID "<<info.dwProcessId<<L".\n";return 0;
    }
    require(remote_module(info.dwProcessId,dll.filename().wstring())==0,"probe already loaded; use --capture or restart the target");
    void* remote_load=reinterpret_cast<void*>(base+(reinterpret_cast<std::uintptr_t>(load)-reinterpret_cast<std::uintptr_t>(owner)));
    const auto load_result=remote_call(process.h,remote_load,dll.wstring());
    std::uintptr_t remote_base=0;for(int retry=0;retry<50&&!remote_base;++retry){remote_base=remote_module(info.dwProcessId,dll.filename().wstring());if(!remote_base)Sleep(20);}
    if(!remote_base){std::cerr<<"LoadLibrary thread result="<<load_result<<'\n';throw std::runtime_error("probe DLL did not load");}
    HMODULE local=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);require(local!=nullptr,"read probe export");
    auto init=GetProcAddress(local,"ArcInitialize");const auto offset=reinterpret_cast<std::uintptr_t>(init)-reinterpret_cast<std::uintptr_t>(local);FreeLibrary(local);require(init!=nullptr,"probe initialization export");
    const auto result=remote_call(process.h,reinterpret_cast<void*>(remote_base+offset),experiment?(vrs_on?L"2x2":L"off"):output.wstring());
    if(result!=0)throw std::runtime_error("probe initialization status "+std::to_string(result));
    std::wcout<<L"{\"pid\":"<<info.dwProcessId<<L",\"initialized\":true,\"mode\":\"observe_only\"}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
