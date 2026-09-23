#pragma once
#include "arc/discovery_budget.hpp"
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <mutex>
#include <atomic>
namespace arc::dx12 {
// Only background compiler/critic callers acquire this. Never on render threads.
inline std::timed_mutex& background_compute_gate(){static std::timed_mutex gate;return gate;}
inline std::atomic<unsigned>& background_quality_waiters(){static std::atomic<unsigned> value{};return value;}
namespace detail {
inline std::uint64_t budget_environment(const char* name,std::uint64_t fallback,std::uint64_t minimum,std::uint64_t maximum) noexcept {
    const char* raw=std::getenv(name);
    if(!raw)return fallback;
    const std::string_view text(raw);
    std::uint64_t parsed{};
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),parsed);
    return error==std::errc{}&&end==text.data()+text.size()&&parsed>=minimum&&parsed<=maximum?parsed:fallback;
}
}
// Read once on first access, before background workers start. Changing an
// override requires process restart; invalid/out-of-range values use defaults.
inline const arc::DiscoveryBudget::Config& background_budget_config() noexcept {
    static const auto config=[] {
        arc::DiscoveryBudget::Config value;
        value.outstanding_jobs=static_cast<unsigned>(detail::budget_environment("ARC_DISCOVERY_MAX_JOBS",value.outstanding_jobs,1,64));
        value.outstanding_bytes=static_cast<std::size_t>(detail::budget_environment("ARC_DISCOVERY_MAX_BYTES",value.outstanding_bytes,1ull<<20,256ull<<20));
        value.capture_window=std::chrono::milliseconds(detail::budget_environment("ARC_DISCOVERY_CAPTURE_MS",value.capture_window.count(),10,1000));
        value.capture_events=detail::budget_environment("ARC_DISCOVERY_CAPTURE_EVENTS",value.capture_events,1000,1'000'000);
        value.capture_bytes=static_cast<std::size_t>(detail::budget_environment("ARC_DISCOVERY_CAPTURE_BYTES",value.capture_bytes,64ull<<10,64ull<<20));
        value.capture_spacing=std::chrono::milliseconds(detail::budget_environment("ARC_DISCOVERY_CAPTURE_SPACING_MS",value.capture_spacing.count(),1000,60'000));
        value.gpu_capture_window=std::chrono::milliseconds(detail::budget_environment("ARC_DISCOVERY_GPU_CAPTURE_MS",value.gpu_capture_window.count(),100,10000));
        value.gpu_capture_events=detail::budget_environment("ARC_DISCOVERY_GPU_CAPTURE_EVENTS",value.gpu_capture_events,1000,1000000);
        value.gpu_capture_bytes=static_cast<std::size_t>(detail::budget_environment("ARC_DISCOVERY_GPU_CAPTURE_BYTES",value.gpu_capture_bytes,64ull<<10,64ull<<20));
        // The existing gate and CPU capture admission support one each.
        value.heavy_workers=1;
        value.cpu_captures=1;
        return value;
    }();
    return config;
}
// Shared quota for queued CPU/GPU analysis. The existing gate serializes the
// one heavy compiler/quality worker while leases account for queued memory.
inline arc::DiscoveryBudget& background_discovery_budget(){static arc::DiscoveryBudget budget(background_budget_config());return budget;}
}
