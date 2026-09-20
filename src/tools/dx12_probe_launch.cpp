#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <stdexcept>
#include <psapi.h>
#include <array>
#include <vector>
#include <cstring>

namespace {
struct Handle{HANDLE h{};~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete;explicit Handle(HANDLE p=nullptr):h(p){};};
void require(bool value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+std::to_string(GetLastError()));}
std::wstring quote(const std::wstring& s){
    std::wstring out=L"\"";std::size_t slashes=0;
    for(wchar_t c:s){if(c==L'\\'){++slashes;continue;}if(c==L'\"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}
    out.append(slashes*2,L'\\');out+=L'\"';return out;
}
std::uintptr_t remote_module(DWORD pid,const std::wstring& name,const std::filesystem::path* expected=nullptr){
    HANDLE raw=INVALID_HANDLE_VALUE;
    for(int retry=0;retry<20;++retry){raw=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);if(raw!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_BAD_LENGTH)break;Sleep(10);}
    Handle snapshot(raw);
    MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(snapshot.h!=INVALID_HANDLE_VALUE&&Module32FirstW(snapshot.h,&entry))do{if(_wcsicmp(entry.szModule,name.c_str())==0){
        if(expected){std::error_code error;require(std::filesystem::equivalent(*expected,entry.szExePath,error),"target has a different ARC DLL; restart with the selected package");}
        return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);}}while(Module32NextW(snapshot.h,&entry));
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
std::uintptr_t early_load_library(HANDLE process,const std::filesystem::path& dll){
    // Before the suspended entry thread has run, the loader's module list and
    // KernelBase need not exist. Ntdll's image is already mapped by Windows.
    // Start a loader-only thread; never patch application instructions.
    HMODULE local=GetModuleHandleW(L"ntdll.dll");const auto loader=GetProcAddress(local,"LdrLoadDll");require(local&&loader,"native loader export");
    std::uintptr_t native_base{};MEMORY_BASIC_INFORMATION memory{};
    for(std::uintptr_t address=0;VirtualQueryEx(process,reinterpret_cast<void*>(address),&memory,sizeof(memory))==sizeof(memory);){
        if(memory.Type==MEM_IMAGE&&memory.State==MEM_COMMIT){wchar_t name[32768]{};
            if(GetMappedFileNameW(process,memory.AllocationBase,name,32768)&&_wcsicmp(std::filesystem::path(name).filename().c_str(),L"ntdll.dll")==0){native_base=reinterpret_cast<std::uintptr_t>(memory.AllocationBase);break;}}
        const auto next=reinterpret_cast<std::uintptr_t>(memory.BaseAddress)+memory.RegionSize;if(next<=address)break;address=next;
    }
    require(native_base!=0,"initial native loader image");
    const auto path=dll.wstring();require(path.size()<32766,"DLL path capacity");
    struct Context {std::uint64_t function;USHORT length,maximum;ULONG padding;std::uint64_t name,result;};
    static_assert(sizeof(Context)==32&&offsetof(Context,length)==8&&offsetof(Context,result)==24);
    const SIZE_T bytes=sizeof(Context)+(path.size()+1)*sizeof(wchar_t);
    void* data=VirtualAllocEx(process,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);require(data!=nullptr,"loader context allocation");
    Context context{native_base+(reinterpret_cast<std::uintptr_t>(loader)-reinterpret_cast<std::uintptr_t>(local)),USHORT(path.size()*2),USHORT((path.size()+1)*2),0,reinterpret_cast<std::uintptr_t>(data)+sizeof(Context),0};
    const unsigned char stub[]{0x49,0x89,0xca,0x48,0x83,0xec,0x28,0x49,0x8b,0x02,0x4d,0x8d,0x42,0x08,0x4d,0x8d,0x4a,0x18,0x31,0xc9,0x31,0xd2,0xff,0xd0,0x48,0x83,0xc4,0x28,0xc3};
    void* code=VirtualAllocEx(process,nullptr,sizeof(stub),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!code){VirtualFreeEx(process,data,0,MEM_RELEASE);throw std::runtime_error("loader code allocation");}
    bool in_flight=false;
    try{SIZE_T written{};require(WriteProcessMemory(process,data,&context,sizeof(context),&written)&&written==sizeof(context),"write loader context");
        require(WriteProcessMemory(process,reinterpret_cast<void*>(context.name),path.c_str(),(path.size()+1)*2,&written)&&written==(path.size()+1)*2,"write loader path");
        require(WriteProcessMemory(process,code,stub,sizeof(stub),&written)&&written==sizeof(stub),"write loader entry");DWORD previous{};
        require(VirtualProtectEx(process,code,sizeof(stub),PAGE_EXECUTE_READ,&previous)&&FlushInstructionCache(process,code,sizeof(stub)),"prepare loader entry");
        Handle thread(CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(code),data,0,nullptr));require(thread.h!=nullptr,"start early loader");in_flight=true;
        require(WaitForSingleObject(thread.h,15000)==WAIT_OBJECT_0,"early loader timeout");in_flight=false;
        DWORD status{};require(GetExitCodeThread(thread.h,&status)&&status==0,"load optimizer DLL");SIZE_T read{};
        require(ReadProcessMemory(process,data,&context,sizeof(context),&read)&&read==sizeof(context)&&context.result,"read loaded module");
        VirtualFreeEx(process,code,0,MEM_RELEASE);VirtualFreeEx(process,data,0,MEM_RELEASE);return context.result;
    }catch(...){if(!in_flight){VirtualFreeEx(process,code,0,MEM_RELEASE);VirtualFreeEx(process,data,0,MEM_RELEASE);}throw;}
}
DWORD call_export(HANDLE process,std::uintptr_t remote,const std::filesystem::path& dll,const char* name,const std::wstring& argument){
    HMODULE local=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);require(local!=nullptr,"read ARC exports");
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(local);const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const char*>(local)+dos->e_lfanew);
    IMAGE_DOS_HEADER remote_dos{};IMAGE_NT_HEADERS64 remote_nt{};SIZE_T read{};
    const bool version=ReadProcessMemory(process,reinterpret_cast<void*>(remote),&remote_dos,sizeof(remote_dos),&read)&&read==sizeof(remote_dos)&&remote_dos.e_magic==IMAGE_DOS_SIGNATURE&&remote_dos.e_lfanew>0&&remote_dos.e_lfanew<1048576&&
        ReadProcessMemory(process,reinterpret_cast<void*>(remote+remote_dos.e_lfanew),&remote_nt,sizeof(remote_nt),&read)&&read==sizeof(remote_nt)&&remote_nt.Signature==IMAGE_NT_SIGNATURE&&
        remote_nt.FileHeader.TimeDateStamp==nt->FileHeader.TimeDateStamp&&remote_nt.OptionalHeader.SizeOfImage==nt->OptionalHeader.SizeOfImage;
    const auto function=GetProcAddress(local,name);const auto offset=reinterpret_cast<std::uintptr_t>(function)-reinterpret_cast<std::uintptr_t>(local);FreeLibrary(local);
    require(version&&function,"matching ARC binary/export required");return remote_call(process,reinterpret_cast<void*>(remote+offset),argument);
}
}
int wmain(int argc,wchar_t** argv)try{
    const std::wstring operation=argc>1?argv[1]:L"";
    const bool starting=operation==L"--attach-starting";
    const bool launch_auto=operation==L"--launch-auto",attach_auto=operation==L"--attach-auto"||starting;
    const bool start_auto=operation==L"--start-auto",stop_auto=operation==L"--stop-auto",target_fps=operation==L"--target-fps";
    const bool policy=operation==L"--policy";
    const bool vrs_on=argc>1&&std::wstring(argv[1])==L"--vrs-2x2";
    const bool vrs_off=argc>1&&std::wstring(argv[1])==L"--vrs-off";
    const bool passive=argc>1&&std::wstring(argv[1])==L"--passive";
    const bool lean=argc>1&&std::wstring(argv[1])==L"--lean";
    const bool profile_stop=argc>1&&std::wstring(argv[1])==L"--gpu-profile-stop";
    const bool profile=argc>1&&std::wstring(argv[1])==L"--gpu-profile";
    const bool experiment=passive||lean||vrs_on||vrs_off||profile_stop||stop_auto;
    const bool features=argc>1&&std::wstring(argv[1])==L"--features";
    const bool image=argc>1&&std::wstring(argv[1])==L"--image";
    const bool timing=argc>1&&std::wstring(argv[1])==L"--measure";
    const bool capture=experiment||profile||features||image||timing||start_auto||target_fps||policy||operation==L"--capture";
    const bool attach=capture||attach_auto||operation==L"--attach";
    if(argc<((launch_auto||attach_auto)?6:experiment?4:attach?5:4)){std::wcerr<<L"Usage: <exe> <DLL> <new metrics.json> [args]\n--launch-auto <exe> <DLL> <new metrics.json> <config.json> [args]\n--attach-auto <PID> <DLL> <new metrics.json> <config.json>\n--start-auto <PID> <DLL> <config.json>\n--stop-auto <PID> <DLL>\n--target-fps <PID> <DLL> <FPS>\n";return 2;}
    const auto dll=std::filesystem::absolute(argv[attach||launch_auto?3:2]);
    const auto output=experiment||target_fps?std::filesystem::path{}:std::filesystem::absolute(argv[attach||launch_auto?4:3]);
    require(std::filesystem::is_regular_file(dll)&&(experiment||target_fps||((start_auto||policy)?std::filesystem::is_regular_file(output):!std::filesystem::exists(output))),"input DLL/configuration or new output path");
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION info{};
    if(attach){
        wchar_t* end=nullptr;const auto pid=wcstoul(argv[2],&end,10);require(end&&!*end&&pid&&pid!=GetCurrentProcessId(),"explicit target PID");info.dwProcessId=pid;
        info.hProcess=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid);require(info.hProcess!=nullptr,"open authorized target");
    }else{
        const auto exe=std::filesystem::absolute(argv[launch_auto?2:1]);require(std::filesystem::is_regular_file(exe),"input executable");
        std::wstring command=quote(exe.wstring());for(int i=launch_auto?6:4;i<argc;++i)command+=L" "+quote(argv[i]);
        require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|(launch_auto?CREATE_SUSPENDED:0),nullptr,exe.parent_path().c_str(),&startup,&info)!=FALSE,"launch owned process");
    }
    Handle process(info.hProcess),main_thread(info.hThread);
    struct Resume {HANDLE thread{};bool pending{};~Resume(){if(pending)ResumeThread(thread);}void run(){if(pending){require(ResumeThread(thread)!=DWORD(-1),"resume target");pending=false;}}} resume{info.hThread,launch_auto};
    std::wcout<<(attach?L"attached_pid=":L"launched_pid=")<<info.dwProcessId<<std::endl;
    BOOL wow{};require(IsWow64Process(process.h,&wow)&&!wow,"x64 target required");
    const auto load=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");require(load!=nullptr,"LoadLibraryW");
    HMODULE owner{};require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(load),&owner)!=FALSE,"loader module");
    wchar_t owner_path[32768]{};require(GetModuleFileNameW(owner,owner_path,32768)>0,"loader module path");
    std::uintptr_t base=0;for(int i=0;i<100&&!base;++i){base=remote_module(info.dwProcessId,std::filesystem::path(owner_path).filename().wstring());if(!base)Sleep(20);}
    require(base!=0||launch_auto||starting,"target loader module unavailable");
    if(capture){
        const auto remote_base=remote_module(info.dwProcessId,dll.filename().wstring(),&dll);require(remote_base!=0,"Target must already have the observer attached");
        if(start_auto||stop_auto||target_fps||policy){
            const auto name=start_auto?"ArcStartOptimizer":stop_auto?"ArcStopOptimizer":target_fps?"ArcSetTargetFps":"ArcExperimentalPolicy";
            const auto argument=stop_auto?L"":target_fps?std::wstring(argv[4]):output.wstring();
            const auto code=call_export(process.h,remote_base,dll,name,argument);require(code==0,"ARC control request refused");return 0;
        }
        HMODULE local=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);require(local!=nullptr,"read capture export");
        auto request=GetProcAddress(local,profile?"ArcRequestGpuProfile":profile_stop?"ArcStopGpuProfile":features?"ArcRequestFeatures":passive?"ArcUsePassiveMode":lean?"ArcUseLeanMode":experiment?"ArcExperimentalVrs":timing?"ArcRequestTiming":image?"ArcRequestImage":"ArcRequestFrame");const auto offset=reinterpret_cast<std::uintptr_t>(request)-reinterpret_cast<std::uintptr_t>(local);FreeLibrary(local);require(request!=nullptr,"capture export");
        const auto argument=experiment?std::wstring(vrs_on?L"2x2":L"off"):(timing||profile)&&argc>5?std::wstring(argv[5])+L"|"+output.wstring():output.wstring();
        const auto code=remote_call(process.h,reinterpret_cast<void*>(remote_base+offset),argument);
        if(code!=0)throw std::runtime_error("Capture request refused, ARC status "+std::to_string(code));std::wcout<<(profile?L"Bounded GPU cost profile":profile_stop?L"GPU profile stopped":features?L"GPU tile features":experiment?(passive?L"Passive observation and cached-list rollback retained":lean?L"Detailed observation disabled":vrs_on?L"Experimental VRS enabled":L"Original command execution restored"):timing?L"Bounded Present cadence":image?L"Image readback":L"Frame graph")<<L" requested for PID "<<info.dwProcessId<<L".\n";return 0;
    }
    const auto existing=remote_module(info.dwProcessId,dll.filename().wstring(),&dll);
    if(existing&&attach_auto){
        const auto config=std::filesystem::absolute(argv[5]);require(std::filesystem::is_regular_file(config),"automatic configuration file");
        require(call_export(process.h,existing,dll,"ArcConfigureRuntime",config.wstring())==0,"configure existing optimizer");
        require(call_export(process.h,existing,dll,"ArcSetMetricsPath",output.wstring())==0,"new session diagnostics");
        require(call_export(process.h,existing,dll,"ArcStartOptimizer",config.wstring())==0,"restart optimizer");return 0;
    }
    require(existing==0,"probe already loaded; use --start-auto or restart the target");
    void* remote_load=reinterpret_cast<void*>(base+(reinterpret_cast<std::uintptr_t>(load)-reinterpret_cast<std::uintptr_t>(owner)));
    std::uintptr_t remote_base=0;DWORD load_result{};
    if(!base&&(launch_auto||starting))remote_base=early_load_library(process.h,dll);
    else load_result=remote_call(process.h,remote_load,dll.wstring());
    for(int retry=0;retry<50&&!remote_base;++retry){remote_base=remote_module(info.dwProcessId,dll.filename().wstring());if(!remote_base)Sleep(20);}
    if(!remote_base){std::cerr<<"LoadLibrary thread result="<<load_result<<'\n';throw std::runtime_error("probe DLL did not load");}
    if(launch_auto||attach_auto){
        const auto config=std::filesystem::absolute(argv[5]);require(std::filesystem::is_regular_file(config),"automatic configuration file");
        require(call_export(process.h,remote_base,dll,"ArcConfigureRuntime",config.wstring())==0,"configure optimizer runtime");
    }
    HMODULE local=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);require(local!=nullptr,"read probe export");
    auto init=GetProcAddress(local,"ArcInitialize");const auto offset=reinterpret_cast<std::uintptr_t>(init)-reinterpret_cast<std::uintptr_t>(local);FreeLibrary(local);require(init!=nullptr,"probe initialization export");
    const auto result=remote_call(process.h,reinterpret_cast<void*>(remote_base+offset),experiment?(vrs_on?L"2x2":L"off"):output.wstring());
    if(result!=0)throw std::runtime_error("probe initialization status "+std::to_string(result));
    if(launch_auto||attach_auto)require(call_export(process.h,remote_base,dll,"ArcUseLeanMode",L"")==0,"enable lightweight observation");
    resume.run();
    std::wcout<<L"{\"pid\":"<<info.dwProcessId<<L",\"initialized\":true,\"mode\":\""<<((launch_auto||attach_auto)?L"automatic":L"observe_only")<<L"\"}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
