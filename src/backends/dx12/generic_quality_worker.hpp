#pragma once
#include "generic_cpu_workers.hpp"
#include "generic_background_budget.hpp"
#include "json.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>
#include <chrono>
#include <stdexcept>
#include <exception>

namespace arc::dx12 {
class QualityWorker {
    using Json=nlohmann::json;
    HANDLE process_{},job_{},input_{},output_{};
    std::unique_ptr<cpu_cost::Registration> accounting_;
    unsigned launches_{},failures_{};std::uint64_t sequence_{};
    static std::wstring quote(const std::wstring& value){std::wstring out=L"\"";unsigned n=0;for(auto c:value){if(c==L'\\'){++n;continue;}if(c==L'"'){out.append(n*2+1,L'\\');out+=c;}else{out.append(n,L'\\');out+=c;}n=0;}out.append(n*2,L'\\');return out+L'"';}
    void close(){if(job_)CloseHandle(job_);job_=nullptr;if(process_)WaitForSingleObject(process_,1000);accounting_.reset();for(auto* h:{&process_,&input_,&output_}){if(*h)CloseHandle(*h);*h=nullptr;}}
    [[noreturn]] void fail(const std::string& reason){++failures_;close();throw std::runtime_error(reason);}
    void launch(const std::filesystem::path& python,const std::filesystem::path& script){
        close();if(failures_>=2)throw std::runtime_error("quality_worker_restart_limit");++launches_;
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE child_in{},child_out{},err{};
        auto cleanup=[&]{for(HANDLE h:{child_in,child_out,err})if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);};
        if(!CreatePipe(&child_in,&input_,&sa,65536)||!CreatePipe(&output_,&child_out,&sa,65536)){cleanup();fail("quality_worker_pipe");}
        SetHandleInformation(input_,HANDLE_FLAG_INHERIT,0);SetHandleInformation(output_,HANDLE_FLAG_INHERIT,0);
        err=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
        job_=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_PROCESS_MEMORY;limits.ProcessMemoryLimit=SIZE_T(1)<<30;
        if(!job_||!SetInformationJobObject(job_,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){cleanup();fail("quality_worker_job");}
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;startup.wShowWindow=SW_HIDE;startup.hStdInput=child_in;startup.hStdOutput=child_out;startup.hStdError=err;
        auto command=quote(python.wstring())+L" "+quote(script.wstring())+L" --serve";PROCESS_INFORMATION pi{};
        const bool created=CreateProcessW(python.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|BELOW_NORMAL_PRIORITY_CLASS,nullptr,script.parent_path().c_str(),&startup,&pi)!=0;
        cleanup();if(!created){fail("quality_worker_launch");}process_=pi.hProcess;
        accounting_=std::make_unique<cpu_cost::Registration>(cpu_cost::Kind::Critic,process_,pi.hThread);
        if(!AssignProcessToJobObject(job_,process_)||ResumeThread(pi.hThread)==DWORD(-1)){TerminateProcess(process_,2);CloseHandle(pi.hThread);fail("quality_worker_start");}CloseHandle(pi.hThread);
    }
    struct Mapping {HANDLE handle{};void* memory{};~Mapping(){if(memory)UnmapViewOfFile(memory);if(handle)CloseHandle(handle);}};
public:
    ~QualityWorker(){close();}
    bool unavailable()const noexcept{return failures_>=2&&!process_;}
    Json assess(const std::filesystem::path& python,const std::filesystem::path& script,Json request,const std::function<bool()>& cancelled,const std::function<void()>& idle={}){
        struct Priority {bool waiting{true};Priority(){++background_quality_waiters();}void acquired(){--background_quality_waiters();waiting=false;}~Priority(){if(waiting)--background_quality_waiters();}} priority;
        std::unique_lock gate(background_compute_gate(),std::defer_lock);
        while(!gate.try_lock_for(std::chrono::milliseconds(50)))if(cancelled())throw std::runtime_error("quality_cancelled");
        priority.acquired();
        if(process_&&WaitForSingleObject(process_,0)!=WAIT_TIMEOUT){++failures_;close();}
        if(!process_)launch(python,script);
        request["schema"]=1;request["request_id"]=++sequence_;request["shared"]=Json::array();
        std::vector<std::unique_ptr<Mapping>> mappings;std::uint64_t total{};
        for(const auto& entry:request.at("paths")){
            const auto path=std::filesystem::u8path(entry.get<std::string>());std::ifstream metadata_file(path);const auto metadata=Json::parse(metadata_file);
            const auto pixel=path.parent_path()/std::filesystem::u8path(metadata.at("pixel_file").get<std::string>());
            if(pixel.lexically_normal().parent_path()!=path.parent_path().lexically_normal())throw std::runtime_error("quality_pixel_path");
            const auto size=std::filesystem::file_size(pixel);total+=size;if(!size||total>256ull*1024*1024)throw std::runtime_error("quality_shared_capacity");
            const auto name="Local\\ARC-quality-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(sequence_)+"-"+std::to_string(mappings.size());
            auto map=std::make_unique<Mapping>();map->handle=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,DWORD(size),name.c_str());
            if(!map->handle||GetLastError()==ERROR_ALREADY_EXISTS)throw std::runtime_error("quality_shared_create");map->memory=MapViewOfFile(map->handle,FILE_MAP_WRITE,0,0,SIZE_T(size));if(!map->memory)throw std::runtime_error("quality_shared_map");
            std::ifstream pixels(pixel,std::ios::binary);pixels.read(static_cast<char*>(map->memory),size);if(!pixels)throw std::runtime_error("quality_shared_read");
            request["shared"].push_back({{"name",name},{"bytes",size}});mappings.push_back(std::move(map));
        }
        const auto line=request.dump()+"\n";if(line.size()>65536)throw std::runtime_error("quality_request_capacity");
        DWORD written{};if(!WriteFile(input_,line.data(),DWORD(line.size()),&written,nullptr)||written!=line.size()){fail("quality_worker_write");}
        std::exception_ptr invalidated;std::string response;const auto deadline=GetTickCount64()+15000;
        while(!cancelled()&&GetTickCount64()<deadline){
            try{if(idle&&!invalidated)idle();}catch(...){invalidated=std::current_exception();} // immutable snapshots can finish; stale result is discarded without restarting Python
            cpu_cost::memory_snapshot();DWORD bytes{};
            if(!PeekNamedPipe(output_,nullptr,0,nullptr,&bytes,nullptr)){fail("quality_worker_exit");}
            if(bytes){char chunk[8192];DWORD read{};if(!ReadFile(output_,chunk,std::min<DWORD>(bytes,sizeof(chunk)),&read,nullptr)){fail("quality_worker_read");}response.append(chunk,read);
                if(response.size()>1024*1024){fail("quality_response_capacity");}
                if(response.find('\n')!=response.npos){Json result;try{result=Json::parse(response);if(!result.is_object()||!result.contains("context")||!result.contains("request_id")||!result["request_id"].is_number_unsigned()||(result.contains("reason")&&!result["reason"].is_string()))fail("quality_response_schema");}catch(const nlohmann::json::exception&){fail("quality_response_json");}if(result.value("request_id",0ull)!=sequence_||result.at("context")!=request.at("context")){fail("quality_stale_response");}
                    if(result.value("reason",std::string{})=="quality_worker_error"){const auto why=result.value("detail",std::string("worker error"));fail("quality_worker_error: "+why);}
                    result["worker_launches"]=launches_;result["worker_failures"]=failures_;result["shared_bytes"]=total;if(invalidated)std::rethrow_exception(invalidated);return result;}}
            if(WaitForSingleObject(process_,20)==WAIT_OBJECT_0){fail("quality_worker_exit");}
        }
        if(cancelled()){close();throw std::runtime_error("quality_cancelled");}fail("quality_worker_timeout");
    }
};
}
