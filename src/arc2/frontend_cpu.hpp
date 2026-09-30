#pragma once

// The normal frontend has no meter state, branch, registration, or timing call.
#ifdef ARC2_MEASURE_CPU
#include "arc/intercept_cpu_meter.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace arc::arc2::cpu {
inline std::atomic<std::uint64_t> site_overflow{};
inline unsigned register_cpu_site(const char* name) noexcept {
    const auto id = arc::InterceptCpuMeter::register_site(name);
    if (id >= arc::InterceptCpuMeter::sites().size()) site_overflow.fetch_add(1, std::memory_order_relaxed);
    return id;
}
#include "generated/cpu_sites.inc"

inline void enable() noexcept {
    static const bool initialized = [] {
        (void)cpu_site(CpuSite::D3D12_QueryInterface);
        arc::InterceptCpuMeter::enable(true);
        return true;
    }();
    (void)initialized;
}
struct MethodScope : arc::InterceptCpuMeter::Scope {
    explicit MethodScope(CpuSite site) noexcept : arc::InterceptCpuMeter::Scope((enable(), true), cpu_site(site)) {}
};
template<class T> T* raw_pointer(T* pointer) noexcept { return pointer; }
template<class T> auto raw_pointer(const T& pointer) noexcept -> decltype(pointer.Get()) { return pointer.Get(); }
template<class P, class F, class... Args>
decltype(auto) app_call(P&& pointer, F&& invoke, Args&&... args) {
    auto* raw = raw_pointer(pointer);
    return arc::original_cpu_call([&]() -> decltype(auto) {
        return std::forward<F>(invoke)(raw, std::forward<Args>(args)...);
    });
}
template<class F, class... Args>
decltype(auto) app_invoke(F&& function, Args&&... args) {
    return arc::original_cpu_call([&]() -> decltype(auto) {
        return std::forward<F>(function)(std::forward<Args>(args)...);
    });
}
struct Frame { std::uint64_t swapchain{}, own_ns{}, excluded_native_ns{}, calls{}; std::int64_t qpc{}; };
inline std::mutex frames_mutex;
inline std::vector<Frame> frames;
inline std::atomic<std::uint64_t> dropped_frames{};
inline void record_present(std::uint64_t swapchain) noexcept {
    try {
        const auto snapshot = arc::InterceptCpuMeter::snapshot();
        LARGE_INTEGER stamp{}; QueryPerformanceCounter(&stamp);
        std::lock_guard lock(frames_mutex);
        if (frames.size() < 65536) frames.push_back({swapchain, snapshot.own_ns, snapshot.excluded_native_ns, snapshot.calls, stamp.QuadPart});
        else dropped_frames.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        dropped_frames.fetch_add(1, std::memory_order_relaxed);
    }
}
struct FrameGuard {
    std::uint64_t swapchain;
    explicit FrameGuard(std::uint64_t id) : swapchain(id) {}
    ~FrameGuard() { record_present(swapchain); }
};
inline void save(const wchar_t* path) {
    if (!path) return;
    std::ofstream out(std::filesystem::path(std::wstring(path) + L".cpu.json"), std::ios::binary);
    if (!out) return;
    out << "{\"schema\":\"arc2-cpu-diagnostic-v1\",\"interpretation\":\"aggregate interceptor wall cost across threads; not critical-path CPU frame time; excluded_native_ns is forwarded-call envelope and may contain nested ARC callbacks\",\"frame_columns\":[\"swapchain\",\"own_ns\",\"excluded_native_ns\",\"calls\",\"qpc\"],\"frames\":[";
    { std::lock_guard lock(frames_mutex);
      bool first = true;
      for (const auto& frame : frames) {
          if (!first) out << ','; first = false;
          out << '[' << frame.swapchain << ',' << frame.own_ns << ','
              << frame.excluded_native_ns << ',' << frame.calls << ',' << frame.qpc << ']';
      }
    }
    out << "],\"dropped_frames\":" << dropped_frames.load(std::memory_order_relaxed) << ",\"sites\":[";
    bool first = true;
    for (const auto& site : arc::InterceptCpuMeter::sites()) {
        const char* name = site.name.load(std::memory_order_relaxed);
        if (!name) continue;
        if (!first) out << ','; first = false;
        out << "[\"" << name << "\"," << site.own_ns.load(std::memory_order_relaxed)
            << ',' << site.calls.load(std::memory_order_relaxed) << ']';
    }
    const auto total = arc::InterceptCpuMeter::snapshot();
    out << "],\"site_overflow\":" << site_overflow.load(std::memory_order_relaxed)
        << ",\"totals\":[" << total.own_ns << ',' << total.excluded_native_ns << ',' << total.calls << "]}";
}
} // namespace arc::arc2::cpu

#define ARC2_CPU_JOIN_IMPL(a,b) a##b
#define ARC2_CPU_JOIN(a,b) ARC2_CPU_JOIN_IMPL(a,b)
#define ARC2_CPU_SCOPE(site) ::arc::arc2::cpu::MethodScope ARC2_CPU_JOIN(arc2_cpu_scope_,__LINE__){::arc::arc2::cpu::CpuSite::site}
#define ARC2_APP_CALL(pointer, method, ...) ::arc::arc2::cpu::app_call(pointer, [](auto* p, auto&&... a) -> decltype(auto) { return p->method(std::forward<decltype(a)>(a)...); } __VA_OPT__(,) __VA_ARGS__)
#define ARC2_APP_INVOKE(function, ...) ::arc::arc2::cpu::app_invoke(function __VA_OPT__(,) __VA_ARGS__)
#define ARC2_CPU_FRAME_GUARD(id) ::arc::arc2::cpu::FrameGuard ARC2_CPU_JOIN(arc2_frame_guard_,__LINE__){id}
#define ARC2_CPU_SAVE(path) ::arc::arc2::cpu::save(path)
#else
#define ARC2_CPU_SCOPE(site)
#define ARC2_APP_CALL(pointer, method, ...) (pointer)->method(__VA_ARGS__)
#define ARC2_APP_INVOKE(function, ...) (function)(__VA_ARGS__)
#define ARC2_CPU_FRAME_GUARD(id)
#define ARC2_CPU_SAVE(path)
#endif
