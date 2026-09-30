#include "present_metrics.hpp"
#include <windows.h>
#include <dxgi.h>
#include <vector>
#include <mutex>
#include <fstream>
#include <filesystem>
namespace arc::arc2 {
namespace {struct Row{std::uint64_t swapchain;std::int64_t begin,native_end,end;std::int32_t result;std::uint32_t flags;};std::mutex lock;std::vector<Row> rows;std::uint64_t dropped{};}
std::int64_t qpc_now(){LARGE_INTEGER q{};QueryPerformanceCounter(&q);return q.QuadPart;}
void present_metric(std::uint64_t swapchain,std::int64_t begin,std::int64_t native_end,std::int32_t result,std::uint32_t flags){std::lock_guard guard(lock);if(rows.empty())rows.reserve(65536);if(rows.size()<65536)rows.push_back({swapchain,begin,native_end,qpc_now(),result,flags});else ++dropped;}
void save_present_metrics(const wchar_t* path){std::lock_guard guard(lock);LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);std::ofstream f(std::filesystem::path(std::wstring(path)+L".frames.csv"));f<<"swapchain,begin_qpc,native_end_qpc,wrapper_end_qpc,hresult,flags,successful_present,qpc_frequency\n";for(const auto&r:rows)f<<r.swapchain<<','<<r.begin<<','<<r.native_end<<','<<r.end<<','<<r.result<<','<<r.flags<<','<<(r.result==S_OK&&!(r.flags&DXGI_PRESENT_TEST))<<','<<frequency.QuadPart<<'\n';std::ofstream meta(std::filesystem::path(std::wstring(path)+L".frames.meta.json"));meta<<"{\"rows\":"<<rows.size()<<",\"dropped\":"<<dropped<<",\"display_fps_measured\":false}";}
}
