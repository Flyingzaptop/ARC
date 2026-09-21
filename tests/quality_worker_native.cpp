#include "generic_quality_worker.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
int wmain(int argc,wchar_t** argv){
    assert(argc==2);using Json=nlohmann::json;
    const auto root=std::filesystem::temp_directory_path()/(L"arc-worker-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-\u0442\u0435\u0441\u0442");
    assert(std::filesystem::create_directory(root));const auto script=root/L"worker.py";
    {std::ofstream file(script);file<<R"(import json,sys,time,os
sys.stdin.reconfigure(encoding='utf-8')
sys.stdout.reconfigure(encoding='utf-8')
for line in sys.stdin:
 r=json.loads(line);mode=r.get('mode','good')
 if mode=='timeout':time.sleep(20)
 if mode=='late':time.sleep(.15)
 if mode=='memory':data=bytearray(2*1024*1024*1024)
 if mode=='stale':r['request_id']+=1
 print(json.dumps(dict(request_id=r['request_id'],context=r['context'],worker_pid=os.getpid(),reason='fixture')),flush=True)
)";}
    auto request=[](const char* mode){return Json{{"paths",Json::array()},{"context",{{"epoch",123}}},{"mode",mode}};};
    auto expect=[&](arc::dx12::QualityWorker& worker,const char* mode,const char* error){bool caught=false;try{worker.assess(argv[1],script,request(mode),[]{return false;});}catch(const std::exception& e){caught=std::string(e.what())==error;}assert(caught);};
    {arc::dx12::QualityWorker worker;const auto first=worker.assess(argv[1],script,request("good"),[]{return false;});
     bool discarded=false;try{worker.assess(argv[1],script,request("late"),[]{return false;},[]{throw std::runtime_error("context_invalidated");});}catch(const std::exception& e){discarded=std::string(e.what())=="context_invalidated";}assert(discarded);
     const auto next=worker.assess(argv[1],script,request("good"),[]{return false;});assert(first["worker_pid"]==next["worker_pid"]&&next["worker_launches"]==1);
     expect(worker,"stale","quality_stale_response");assert(!worker.unavailable());const auto restarted=worker.assess(argv[1],script,request("good"),[]{return false;});assert(restarted["worker_launches"]==2);
     expect(worker,"stale","quality_stale_response");assert(worker.unavailable());expect(worker,"good","quality_worker_restart_limit");}
    {arc::dx12::QualityWorker worker;for(unsigned i=0;i<3;++i){const auto start=GetTickCount64();bool cancelled=false;try{worker.assess(argv[1],script,request("timeout"),[&]{return GetTickCount64()-start>100;});}catch(const std::exception& e){cancelled=std::string(e.what())=="quality_cancelled";}assert(cancelled&&!worker.unavailable());}
     const auto healthy=worker.assess(argv[1],script,request("good"),[]{return false;});assert(healthy["worker_failures"]==0);}
    {arc::dx12::QualityWorker worker;const auto start=GetTickCount64();expect(worker,"timeout","quality_worker_timeout");assert(GetTickCount64()-start<18000&&!worker.unavailable());}
    {arc::dx12::QualityWorker worker;bool bounded=false;try{worker.assess(argv[1],script,request("memory"),[]{return false;});}catch(const std::exception& e){bounded=std::string(e.what())=="quality_worker_exit";}assert(bounded);}
    std::filesystem::remove(script);std::filesystem::remove(root);
    std::cout<<"Resident IPC, Unicode paths, stale replies, context cancellation, restart budget, timeout and 1GiB job limit PASS\n";
}
