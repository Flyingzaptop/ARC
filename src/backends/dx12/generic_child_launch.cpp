#include "generic_child_launch.hpp"
#include "MinHook.h"
#include "json.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>
#include <string>

namespace arc::dx12::children {
namespace {
using Json=nlohmann::json;
using CreateFn=decltype(&CreateProcessW);
CreateFn original{};
std::mutex mutex;
Json settings;
std::atomic<bool> enabled{};
thread_local bool internal{};
std::wstring quote(const std::wstring& s){std::wstring out=L"\"";unsigned n=0;for(auto c:s){if(c==L'\\'){++n;continue;}if(c==L'"'){out.append(n*2+1,L'\\');out+=c;}else{out.append(n,L'\\');out+=c;}n=0;}out.append(n*2,L'\\');return out+L'"';}
std::string utf8(const std::filesystem::path& p){const auto s=p.u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
bool inside(const std::filesystem::path& file,const std::filesystem::path& root){
    const auto a=std::filesystem::weakly_canonical(file),b=std::filesystem::canonical(root);
    auto i=a.begin();for(auto j=b.begin();j!=b.end();++j,++i)if(i==a.end()||_wcsicmp(i->c_str(),j->c_str()))return false;return i!=a.end();
}
void prepare(const PROCESS_INFORMATION& child){
    Json config;{std::lock_guard lock(mutex);config=settings;}
    wchar_t path[32768]{};DWORD size=32768;
    if(!QueryFullProcessImageNameW(child.hProcess,0,path,&size))return;
    if(!inside(path,std::filesystem::u8path(config.at("launch_scope").get<std::string>())))return;
    const auto depth=config.value("launch_depth",0u);if(depth>=4)return;
    const auto root=std::filesystem::u8path(config.at("session_root").get<std::string>());
    const auto directory=root/(L"process-"+std::to_wstring(child.dwProcessId));
    if(!std::filesystem::create_directory(directory))return;
    config["launch_depth"]=depth+1;config["output"]=utf8(directory/L"automatic");
    const auto file=directory/L"config.json",metrics=directory/L"arc.json";
    {std::ofstream out(file);out<<config.dump(2);if(!out)return;}
    const auto helper=std::filesystem::u8path(config.at("launch_helper").get<std::string>());
    const auto dll=helper.parent_path()/L"arc-dx12-probe.dll";
    auto command=quote(helper.wstring())+L" --attach-starting "+std::to_wstring(child.dwProcessId)+L" "+quote(dll.wstring())+L" "+quote(metrics.wstring())+L" "+quote(file.wstring());
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE log=CreateFileW((directory/L"launcher.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;startup.wShowWindow=SW_HIDE;startup.hStdOutput=startup.hStdError=log;startup.hStdInput=input;
    PROCESS_INFORMATION worker{};
    const bool started=log!=INVALID_HANDLE_VALUE&&input!=INVALID_HANDLE_VALUE&&original(helper.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,helper.parent_path().c_str(),&startup,&worker);
    if(log!=INVALID_HANDLE_VALUE)CloseHandle(log);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
    if(!started)return;
    // The application's entry thread remains suspended while the helper installs
    // observation. This wait occurs only at process creation, never per frame.
    const auto waited=WaitForSingleObject(worker.hProcess,45000);DWORD result=1;
    if(waited==WAIT_OBJECT_0)GetExitCodeProcess(worker.hProcess,&result);
    CloseHandle(worker.hThread);CloseHandle(worker.hProcess);
    std::ofstream outcome(directory/L"handoff.json");outcome<<Json{{"pid",child.dwProcessId},{"parent_pid",GetCurrentProcessId()},{"image",utf8(path)},{"initialized_before_resume",waited==WAIT_OBJECT_0&&result==0},{"helper_completed",waited==WAIT_OBJECT_0},{"helper_exit_code",result}}.dump(2);
}
BOOL WINAPI create(LPCWSTR app,LPWSTR line,LPSECURITY_ATTRIBUTES pa,LPSECURITY_ATTRIBUTES ta,BOOL inherit,DWORD flags,LPVOID env,LPCWSTR cwd,LPSTARTUPINFOW startup,LPPROCESS_INFORMATION info){
    if(!enabled||internal||(flags&(DEBUG_PROCESS|DEBUG_ONLY_THIS_PROCESS))||!info)return original(app,line,pa,ta,inherit,flags,env,cwd,startup,info);
    internal=true;
    const auto ok=original(app,line,pa,ta,inherit,flags|CREATE_SUSPENDED,env,cwd,startup,info);const auto error=GetLastError();
    if(ok){try{prepare(*info);}catch(...){}if(!(flags&CREATE_SUSPENDED))ResumeThread(info->hThread);}
    internal=false;SetLastError(error);return ok;
}
}
bool configure(const wchar_t* path)noexcept{
    try{std::ifstream file(path);auto config=Json::parse(file);
        if(!config.contains("launch_scope")){enabled=false;return true;}
        for(const char* key:{"launch_scope","launch_helper","session_root"}){auto p=std::filesystem::u8path(config.at(key).get<std::string>());if(!p.is_absolute()||!std::filesystem::exists(p))return false;}
        std::lock_guard lock(mutex);settings=std::move(config);enabled=true;return true;
    }catch(...){return false;}
}
bool install()noexcept{
    if(!enabled)return true;
    const auto target=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"CreateProcessW");
    return target&&MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(&create),reinterpret_cast<void**>(&original))==MH_OK;
}
void stop()noexcept{enabled=false;}
}
