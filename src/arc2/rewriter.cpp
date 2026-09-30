#include "arc/arc2/frontend.hpp"
#include <atomic>
#include <cstring>
namespace arc::arc2 {
namespace {
std::atomic<unsigned long long> accepted{},rejected{};
std::atomic<int> override_mode{-1};
bool admit(std::uint64_t, bool redundant) noexcept {
 static const bool optimize=[] { char mode[32]{};GetEnvironmentVariableA("ARC2_MODE",mode,32);return std::strcmp(mode,"optimize")==0; }();
 const auto override=override_mode.load();const bool enabled=optimize&&(override<0||override!=0);
 if(!enabled||!redundant){++rejected;return false;}++accepted;return true;
}
struct Register { Register(){set_clear_skip_admit(&admit);} } registration;
}
}
extern "C" __declspec(dllexport) unsigned long long WINAPI Arc2AcceptedActions(){return arc::arc2::accepted.load();}
extern "C" __declspec(dllexport) unsigned long long WINAPI Arc2RejectedActions(){return arc::arc2::rejected.load();}

extern "C" __declspec(dllexport) void WINAPI Arc2SetOptimizationEnabled(BOOL enabled){arc::arc2::override_mode.store(enabled?1:0);}
