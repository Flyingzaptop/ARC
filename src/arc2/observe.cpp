#include "observe.hpp"
#include "arc/arc2/runtime.hpp"
#include "arc/arc2/bridge.hpp"
#include "arc/resource_semantics.hpp"
#include "arc/scene_understanding.hpp"
#include <windows.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <fstream>
#include <filesystem>
namespace arc::arc2 {
namespace {
struct Observer {
 std::mutex mutex;std::condition_variable wake;std::thread worker;bool stop{};
 unsigned long long passes{},resources{},predictions{},failures{};double cpu_ms{};
 void start(){std::lock_guard lock(mutex);if(worker.joinable()||stop)return;char mode[32]{};GetEnvironmentVariableA("ARC2_MODE",mode,32);if(std::strcmp(mode,"observe")&&std::strcmp(mode,"optimize"))return;worker=std::thread([this]{run();});}
 void run(){SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);for(;;){std::unique_lock lock(mutex);if(wake.wait_for(lock,std::chrono::milliseconds(500),[&]{return stop;}))return;lock.unlock();auto begin=std::chrono::steady_clock::now();try{auto ir=runtime().snapshot();ResourceGraph graph;feed_resource_graph(ir,graph);auto classified=ResourceSemanticInferencer{}.classify_all(graph);auto scene=SceneUnderstandingInferencer{}.summarize(graph);(void)scene;lock.lock();++passes;resources=graph.resource_count();predictions=classified.size();cpu_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();}catch(...){lock.lock();++failures;}}}
 void finish(){{std::lock_guard lock(mutex);stop=true;wake.notify_all();}if(worker.joinable())worker.join();}
 ~Observer(){finish();}
};
Observer& observer(){static Observer* o=new Observer;return *o;}
}
void observe_tick(){observer().start();}
void stop_observer(){observer().finish();}
void save_observer(const wchar_t* path){auto& o=observer();std::lock_guard lock(o.mutex);std::ofstream f(std::filesystem::path(std::wstring(path)+L".analysis.json"));f<<"{\"passes\":"<<o.passes<<",\"resource_count\":"<<o.resources<<",\"classifications\":"<<o.predictions<<",\"failures\":"<<o.failures<<",\"analysis_wall_ms\":"<<o.cpu_ms<<",\"note\":\"background graph and semantic analysis; no GPU work\"}";}
}
