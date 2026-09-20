#include "generic_worker_placement.hpp"
#include "json.hpp"
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
using Json=nlohmann::json;
namespace placement=arc::dx12::placement;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::vector<ULONG> sets(bool process){ULONG count{};auto call=[&](ULONG* p,ULONG n){return process?GetProcessDefaultCpuSets(GetCurrentProcess(),p,n,&count):GetThreadSelectedCpuSets(GetCurrentThread(),p,n,&count);};
    check(call(nullptr,0)||GetLastError()==ERROR_INSUFFICIENT_BUFFER,"CPU set size");std::vector<ULONG> values(count);if(count)check(call(values.data(),count)!=0,"CPU sets");std::sort(values.begin(),values.end());return values;}
Json status(){std::ostringstream out;placement::snapshot(out);return Json::parse(out.str());}
std::vector<ULONG> all_ids(){ULONG bytes{};GetSystemCpuSetInformation(nullptr,0,&bytes,GetCurrentProcess(),0);std::vector<std::byte> storage(bytes);check(GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(storage.data()),bytes,&bytes,GetCurrentProcess(),0)!=0,"Topology");std::vector<ULONG> result;
    for(ULONG i=0;i<bytes;){const auto* item=reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(storage.data()+i);if(item->Type==CpuSetInformation)result.push_back(item->CpuSet.Id);i+=item->Size;}std::sort(result.begin(),result.end());return result;}
int wmain(int argc,wchar_t** argv)try{
    if(argc>=2&&std::wstring(argv[1])==L"--child"){
        std::vector<ULONG> expected;for(int i=2;i<argc;++i)expected.push_back(std::stoul(argv[i]));std::sort(expected.begin(),expected.end());check(sets(true)==expected,"Child worker settings before execution");return 0;
    }
    const auto original_process=sets(true),original_thread=sets(false);PROCESSOR_NUMBER original_ideal{};check(GetThreadIdealProcessorEx(GetCurrentThread(),&original_ideal)!=0,"Original ideal");
    DWORD_PTR original_affinity{},system_affinity{};check(GetProcessAffinityMask(GetCurrentProcess(),&original_affinity,&system_affinity)!=0,"Original hard affinity");
    SetEnvironmentVariableW(L"ARC_WORKER_PLACEMENT",L"normal");SetEnvironmentVariableW(L"ARC_WORKER_ALLOW_PARTITION",L"1");check(placement::initialize(),"Initialize placement");
    if(!status().at("supported").get<bool>()){std::cout<<"No qualified topology: normal fallback verified\n";return 0;}
    placement::Lease self;
    check(!placement::configure(L"invalid"),"Unknown mode rejected");check(sets(true)==original_process&&sets(false)==original_thread,"Normal leaves settings intact");
    check(placement::configure(L"prefer"),"Prefer");check(sets(true)==original_process&&sets(false)==original_thread,"Hint does not restrict CPU sets");
    if(!placement::configure(L"core")){std::cerr<<status().dump()<<"\n";throw std::runtime_error("Core");}auto wanted=status().at("worker_cpu_sets").get<std::vector<ULONG>>();std::sort(wanted.begin(),wanted.end());check(sets(false)==wanted,"Both SMT siblings assigned");check(sets(true)==original_process,"Core leaves game CPU sets intact");
    const auto logicals=status().at("worker_logicals").get<std::vector<unsigned>>();const auto group=status().at("worker_group").get<unsigned>();
    for(unsigned i=0;i<8;++i){Sleep(1);PROCESSOR_NUMBER current{};GetCurrentProcessorNumberEx(&current);check(current.Group==group&&std::find(logicals.begin(),logicals.end(),current.Number)!=logicals.end(),"Actual worker runs on selected physical core");}
    wchar_t exe[32768]{};check(GetModuleFileNameW(nullptr,exe,32768)!=0,"Executable path");std::wstring command=L"\""+std::wstring(exe)+L"\" --child";for(auto id:wanted)command+=L" "+std::to_wstring(id);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};check(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&child)!=0,"Child creation");
    {placement::Lease child_policy(child.hProcess,child.hThread);check(ResumeThread(child.hThread)!=DWORD(-1),"Child resume");const auto waited=WaitForSingleObject(child.hProcess,5000);if(waited!=WAIT_OBJECT_0){TerminateProcess(child.hProcess,2);WaitForSingleObject(child.hProcess,1000);}check(waited==WAIT_OBJECT_0,"Bounded child completion");DWORD code{};check(GetExitCodeProcess(child.hProcess,&code)&&code==0,"Child policy verification");}
    CloseHandle(child.hThread);CloseHandle(child.hProcess);
    if(status().at("physical_cores").get<unsigned>()>=6){
        check(placement::configure(L"partition"),"Default partition");const auto game=sets(true);check(!game.empty(),"Game keeps processors");for(auto id:wanted)check(std::find(game.begin(),game.end(),id)==game.end(),"Worker siblings excluded from game default");
        bool inherited=false;std::thread game_thread([&]{inherited=sets(false).empty()&&sets(true)==game;for(unsigned i=0;i<8;++i){Sleep(1);PROCESSOR_NUMBER current{};GetCurrentProcessorNumberEx(&current);inherited&=current.Group!=group||std::find(logicals.begin(),logicals.end(),current.Number)==logicals.end();}});game_thread.join();check(inherited,"Ordinary thread uses game defaults outside worker core");
    }
    check(placement::configure(L"normal"),"Restore normal");PROCESSOR_NUMBER restored{};check(GetThreadIdealProcessorEx(GetCurrentThread(),&restored)!=0,"Read restored ideal");
    check(sets(true)==original_process&&sets(false)==original_thread&&restored.Group==original_ideal.Group&&restored.Number==original_ideal.Number,"Exact original settings restored");
    DWORD_PTR affinity{},system{};check(GetProcessAffinityMask(GetCurrentProcess(),&affinity,&system)&&affinity==original_affinity,"Hard process affinity untouched");check(status().at("failures")==0,"Healthy native placement");
    DWORD_PTR reduced=original_affinity;for(auto logical:logicals)reduced&=~(DWORD_PTR(1)<<logical);
    if(reduced){check(SetProcessAffinityMask(GetCurrentProcess(),reduced)!=0,"Test-owned hard affinity change");check(!placement::configure(L"core")&&status().at("mode")=="normal","External hard-affinity change refuses placement");check(SetProcessAffinityMask(GetCurrentProcess(),original_affinity)!=0,"Restore test hard affinity");}
    check(placement::configure(L"adaptive"),"Adaptive start");check(!placement::configure(L"invalid")&&placement::adaptive(),"Invalid request cannot stop adaptive recovery");
    for(unsigned i=0;i<500&&status().at("adaptive_windows").get<unsigned>()==0;++i){placement::present(reinterpret_cast<void*>(1));placement::collect(1,100);Sleep(1);}
    check(status().at("mode")=="prefer","Adaptive candidate applied on background collection");placement::collect(2,100);
    check(status().at("mode")=="normal"&&sets(true)==original_process&&sets(false)==original_thread,"GPU policy change invalidates placement trial and restores original");
    const auto windows=status().at("adaptive_windows").get<unsigned>();
    for(unsigned i=0;i<250&&status().at("adaptive_windows").get<unsigned>()==windows;++i){placement::present(reinterpret_cast<void*>(1));placement::collect(2,100);Sleep(1);}
    check(status().at("mode")=="prefer","Second adaptive candidate");Sleep(2100);placement::collect(2,100);
    check(status().at("mode")=="normal","No Present progress restores original placement");check(placement::configure(L"normal")&&!placement::adaptive(),"Explicit stop restores and disables adaptive work");
    if(status().at("physical_cores").get<unsigned>()>=6){
        check(placement::configure(L"partition"),"Second partition");const auto external=all_ids();check(SetProcessDefaultCpuSets(GetCurrentProcess(),external.data(),static_cast<ULONG>(external.size()))!=0,"Simulate external change");
        check(placement::configure(L"normal"),"Restore ARC after external change");check(sets(true)==external,"Do not overwrite external changes");check(!placement::configure(L"partition"),"Conflicting reservation refused");
        check(SetProcessDefaultCpuSets(GetCurrentProcess(),original_process.empty()?nullptr:original_process.data(),static_cast<ULONG>(original_process.size()))!=0,"Restore test-owned external change");
    }
    std::cout<<"Native worker hint/core/partition, child inheritance, rollback, ownership conflicts and unchanged hard affinity passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
