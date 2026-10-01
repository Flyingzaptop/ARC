#pragma once
// Testbed-only Present-return timestamps; no engine semantics or optimizer data.
// Flushes on normal exit so the render thread performs no file I/O.
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>

namespace arc2_native_present {
struct Row {
    int64_t begin_qpc;
    int64_t end_qpc;
    int32_t hresult;
    uint32_t flags;
};
struct Recorder {
    static constexpr uint32_t capacity = 131072;
    std::array<Row, capacity> rows{};
    std::atomic<uint32_t> count{0};
    LARGE_INTEGER frequency{};
    Recorder() { QueryPerformanceFrequency(&frequency); }
    ~Recorder() {
        char path[32768]{};
        DWORD n = GetEnvironmentVariableA("ARC2_NATIVE_FRAMES_CSV", path, sizeof(path));
        if (!n || n >= sizeof(path)) return;
        FILE* file = nullptr;
        if (fopen_s(&file, path, "wb") || !file) return;
        std::fprintf(file, "index,begin_qpc,end_qpc,hresult,flags,successful_present,qpc_frequency\n");
        uint32_t length = count.load(std::memory_order_acquire);
        uint32_t stored = length < capacity ? length : capacity;
        for (uint32_t i = 0; i < stored; ++i) {
            const Row& r = rows[i];
            std::fprintf(file, "%u,%lld,%lld,%ld,%u,%u,%lld\n", i,
                         static_cast<long long>(r.begin_qpc),
                         static_cast<long long>(r.end_qpc),
                         static_cast<long>(r.hresult), r.flags,
                         SUCCEEDED(r.hresult) ? 1u : 0u,
                         static_cast<long long>(frequency.QuadPart));
        }
        std::fclose(file);
    }
};
inline Recorder& recorder() { static Recorder value; return value; }
inline int64_t now() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
inline void record(int64_t begin, HRESULT hr, uint32_t flags) {
    Recorder& state = recorder();
    uint32_t index = state.count.fetch_add(1, std::memory_order_relaxed);
    if (index < Recorder::capacity) state.rows[index] = {begin, now(), hr, flags};
}
}
