// Isolated original-code CPU replay child. Never injects into the source process.
// Build with MSVC x64 and context.asm; launch only through the timeout wrapper.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winnt.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct alignas(16) EntryState {
    uint64_t rip, rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, flags;
    uint32_t mxcsr;
    uint8_t pad[12];
    uint8_t xmm[16][16];
};
struct alignas(16) HostState {
    uint64_t rsp, rbx, rbp, rsi, rdi, r12, r13, r14, r15;
    uint32_t mxcsr;
    uint8_t pad[4];
    uint8_t xmm6to15[10][16];
};
static_assert(offsetof(EntryState, flags) == 136);
static_assert(offsetof(EntryState, mxcsr) == 144);
static_assert(offsetof(EntryState, xmm) == 160);
static_assert(offsetof(HostState, xmm6to15) == 80);
extern "C" HostState g_hostState = {};
extern "C" EntryState g_exitState = {};
extern "C" void StartIsolated(const EntryState*);
extern "C" void ReturnStub();

namespace {
constexpr uint64_t kMaxRegion = 1ull << 30;
constexpr uint32_t kMaxRegions = 4096;
constexpr uint32_t kMaxInstructions = 100000;
constexpr uint64_t kU64Max = std::numeric_limits<uint64_t>::max();
volatile LONG g_inReplay = 0;
volatile LONG g_trapCode = 0;
volatile uint64_t g_trapRip = 0;
volatile DWORD g_exceptionCode = 0;
volatile ULONG g_exceptionParameters = 0;
volatile uint64_t g_exceptionInfo0 = 0;
volatile uint64_t g_exceptionInfo1 = 0;
CONTEXT g_faultContext = {};
uint64_t g_moduleBase = 0;
std::vector<uint64_t> g_trapAddresses;

#include "replay_memory.hpp"

uint64_t readQword(uint64_t address, const Pack& p, const Module& module) {
    const bool covered = std::any_of(p.regions.begin(), p.regions.end(),
        [address](const Region& r) {
            return r.base <= address && address + 8 <= r.base + r.size;
        });
    if (!covered) throw std::runtime_error("header_read_outside_snapshot");
    (void)module;
    return *reinterpret_cast<const uint64_t*>(address);
}

void checkCapacity(const Pack& p, const Module& module) {
    const uint64_t cursor = readQword(p.owner + p.cursorOffset, p, module);
    const uint64_t capacity = readQword(p.owner + p.capacityOffset, p, module);
    const uint64_t countBytes = static_cast<uint64_t>(p.packetCount) * p.stride;
    if (!cursor || cursor > capacity ||
        (capacity - cursor) % p.stride ||
        countBytes > kU64Max - cursor || cursor + countBytes > capacity)
        throw std::runtime_error("packet_no_growth_guard_failed");
    const uint64_t headerEnd = p.owner +
        std::max(p.cursorOffset, p.capacityOffset) + 8;
    if (p.owner < cursor + countBytes && cursor < headerEnd)
        throw std::runtime_error("append_header_aliases_output");
    const bool outputCovered = std::any_of(p.regions.begin(), p.regions.end(),
        [cursor, capacity](const Region& r) {
            return r.base <= cursor && capacity <= r.base + r.size;
        });
    if (!outputCovered) throw std::runtime_error("output_region_not_contiguous");
}

void checkScatterOutput(const Pack& p, const Module& module) {
    if (p.version != 3) throw std::runtime_error("not_scatter_pack");
    const uint64_t observedBase =
        readQword(p.outputPointerAddress, p, module);
    if (observedBase != p.outputBase || p.outputStart < observedBase ||
        p.outputStart >= p.outputEnd ||
        (p.outputStart - observedBase) % p.stride ||
        (p.outputEnd - p.outputStart) % p.stride)
        throw std::runtime_error("scatter_output_pointer_or_alignment_changed");
    const auto outputRegion = std::find_if(p.regions.begin(), p.regions.end(),
        [&p](const Region& r) {
            return r.base <= p.outputStart &&
                   p.outputEnd <= r.base + r.size;
        });
    if (outputRegion == p.regions.end())
        throw std::runtime_error("scatter_output_outside_snapshot");
    if (outputRegion->protect != (PAGE_READWRITE | PAGE_WRITECOMBINE))
        throw std::runtime_error("scatter_output_not_writecombine");
    MEMORY_BASIC_INFORMATION mapped{};
    if (!VirtualQuery(reinterpret_cast<void*>(p.outputStart), &mapped,
                      sizeof(mapped)) ||
        (mapped.Protect & (PAGE_READWRITE | PAGE_WRITECOMBINE)) !=
        (PAGE_READWRITE | PAGE_WRITECOMBINE))
        throw std::runtime_error("scatter_output_actual_protection_differs");
}

void checkCode(const Pack& p, const Module& m) {
    for (const auto& x : p.code) {
        if (x.rva > m.size || x.bytes.size() > m.size - x.rva ||
            std::memcmp(reinterpret_cast<void*>(m.base + x.rva),
                        x.bytes.data(), x.bytes.size()))
            throw std::runtime_error("mapped_machine_code_differs_from_capture");
    }
}

uint64_t nearLiteral(uint64_t moduleBase, uint32_t moduleSize) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uint64_t granularity = info.dwAllocationGranularity;
    const uint64_t start = (moduleBase + moduleSize + granularity - 1) &
                           ~(granularity - 1);
    for (uint64_t delta = 0; delta < 0x70000000ull; delta += granularity) {
        for (int sign : {1, -1}) {
            uint64_t candidate = sign > 0 ? start + delta : start - delta;
            if (candidate < granularity) continue;
            void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), 4096,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (reinterpret_cast<uint64_t>(p) == candidate) return candidate;
        }
    }
    throw std::runtime_error("no_nearby_GS_literal_page");
}

void patchCode(const Pack& p, const Module& m, uint64_t literal) {
    if (!p.gsSites.empty()) {
        if (!literal) throw std::runtime_error("missing_GS_literal");
        *reinterpret_cast<uint64_t*>(literal) = p.tlsPointer;
    }
    for (uint32_t rva : p.gsSites) {
        if (rva + 9 > m.size) throw std::runtime_error("GS_patch_out_of_image");
        auto* site = reinterpret_cast<uint8_t*>(m.base + rva);
        const int64_t disp = static_cast<int64_t>(literal) -
                             static_cast<int64_t>(m.base + rva + 7);
        if (disp < std::numeric_limits<int32_t>::min() ||
            disp > std::numeric_limits<int32_t>::max())
            throw std::runtime_error("GS_literal_out_of_reach");
        site[0] = 0x48; site[1] = 0x8b; site[2] = 0x05;
        int32_t displacement = static_cast<int32_t>(disp);
        std::memcpy(site + 3, &displacement, 4);
        site[7] = 0x90; site[8] = 0x90;
    }
    g_trapAddresses.clear();
    for (uint32_t rva : p.trapSites) {
        if (rva >= m.size) throw std::runtime_error("trap_out_of_image");
        *reinterpret_cast<uint8_t*>(m.base + rva) = 0xcc;
        g_trapAddresses.push_back(m.base + rva);
    }
    if (p.loopEnd + 14 > m.size)
        throw std::runtime_error("return_redirect_out_of_image");
    auto* end = reinterpret_cast<uint8_t*>(m.base + p.loopEnd);
    // FF 25 00000000; absolute qword target.
    const uint8_t jump[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(end, jump, 6);
    const uint64_t target = reinterpret_cast<uint64_t>(&ReturnStub);
    std::memcpy(end + 6, &target, 8);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(m.base), m.size);
}

EntryState makeEntry(const Pack& p) {
    CONTEXT c{};
    std::memcpy(&c, p.context.data(), sizeof(c));
    if (c.Rip != p.moduleBase + p.loopEntry ||
        c.Rsp == 0 || !(c.ContextFlags & CONTEXT_CONTROL) ||
        !(c.ContextFlags & CONTEXT_INTEGER) ||
        !(c.ContextFlags & CONTEXT_FLOATING_POINT))
        throw std::runtime_error("entry_context_incomplete_or_wrong_PC");
    EntryState x{};
    x.rip = c.Rip; x.rax = c.Rax; x.rcx = c.Rcx; x.rdx = c.Rdx;
    x.rbx = c.Rbx; x.rsp = c.Rsp; x.rbp = c.Rbp;
    x.rsi = c.Rsi; x.rdi = c.Rdi; x.r8 = c.R8; x.r9 = c.R9;
    x.r10 = c.R10; x.r11 = c.R11; x.r12 = c.R12; x.r13 = c.R13;
    x.r14 = c.R14; x.r15 = c.R15; x.flags = c.EFlags;
    x.mxcsr = c.MxCsr;
    std::memcpy(x.xmm, &c.Xmm0, sizeof(x.xmm));
    return x;
}

LONG CALLBACK exceptionHandler(PEXCEPTION_POINTERS ep) {
    if (!InterlockedCompareExchange(&g_inReplay, 0, 0))
        return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t rip = ep->ContextRecord->Rip;
    const bool knownTrap = std::find(g_trapAddresses.begin(), g_trapAddresses.end(),
                                     rip - 1) != g_trapAddresses.end();
    g_trapRip = rip;
    g_exceptionCode = ep->ExceptionRecord->ExceptionCode;
    g_exceptionParameters = ep->ExceptionRecord->NumberParameters;
    g_exceptionInfo0 = ep->ExceptionRecord->NumberParameters > 0 ?
        ep->ExceptionRecord->ExceptionInformation[0] : 0;
    g_exceptionInfo1 = ep->ExceptionRecord->NumberParameters > 1 ?
        ep->ExceptionRecord->ExceptionInformation[1] : 0;
    std::memcpy(&g_faultContext, ep->ContextRecord, sizeof(g_faultContext));
    InterlockedExchange(&g_trapCode, knownTrap ? 1 : 2);
    ep->ContextRecord->Rip = reinterpret_cast<uint64_t>(&ReturnStub);
    return EXCEPTION_CONTINUE_EXECUTION;
}

#include "replay_verify.hpp"

int reportTrap(const Pack& p, const Module& module, uint32_t run,
               const char* mode) {
    const uint64_t fault = g_exceptionInfo1;
    const bool inSnapshot = std::any_of(
        p.regions.begin(), p.regions.end(),
        [fault](const Region& r) {
            return r.base <= fault && fault < r.base + r.size;
        });
    const bool inModule =
        module.base <= fault && fault < module.base + module.size;
    std::cerr << "{\"replay_trap\":true,\"mode\":\"" << mode
              << "\",\"run\":" << run
              << ",\"trap_class\":" << g_trapCode
              << ",\"rip_hex\":\"0x" << std::hex << g_trapRip
              << "\",\"exception_hex\":\"0x" << g_exceptionCode
              << "\",\"fault_hex\":\"0x" << fault
              << "\",\"access_kind\":" << std::dec << g_exceptionInfo0
              << ",\"exception_parameters\":" << g_exceptionParameters
              << ",\"fault_in_snapshot\":" << (inSnapshot ? "true" : "false")
              << ",\"fault_in_module\":" << (inModule ? "true" : "false")
              << ",\"rax_hex\":\"0x" << std::hex << g_faultContext.Rax
              << "\",\"rcx_hex\":\"0x" << g_faultContext.Rcx
              << "\",\"rdx_hex\":\"0x" << g_faultContext.Rdx
              << "\",\"rsi_hex\":\"0x" << g_faultContext.Rsi
              << "\",\"r15_hex\":\"0x" << g_faultContext.R15
              << "\",\"rsi_plus_r15_hex\":\"0x"
              << (g_faultContext.Rsi + g_faultContext.R15)
              << "\",\"r12_hex\":\"0x" << g_faultContext.R12
              << "\",\"r13_hex\":\"0x" << g_faultContext.R13
              << "\",\"rsp_hex\":\"0x" << g_faultContext.Rsp
              << "\"}\n" << std::dec;
    return 3;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: replay.exe prepared-pack.bin [restored_1_to_10] [warm_0_to_10]\n";
        return 2;
    }
    try {
        uint32_t repeats = 1;
        uint32_t warmRepeats = 0;
        if (argc >= 3) {
            wchar_t* end = nullptr;
            const unsigned long parsed = wcstoul(argv[2], &end, 10);
            if (!end || *end || parsed < 1 || parsed > 10)
                throw std::runtime_error("invalid_repeat_count");
            repeats = static_cast<uint32_t>(parsed);
        }
        if (argc == 4) {
            wchar_t* end = nullptr;
            const unsigned long parsed = wcstoul(argv[3], &end, 10);
            if (!end || *end || parsed > 10)
                throw std::runtime_error("invalid_warm_repeat_count");
            warmRepeats = static_cast<uint32_t>(parsed);
        }
        const Pack p = readPack(argv[1]);
        const Module module = mapPe(p);
        restoreRegions(p, module);
        checkCode(p, module);
        if (p.version == 2) checkCapacity(p, module);
        else checkScatterOutput(p, module);
        const uint64_t initialCursor = p.version == 2 ?
            readQword(p.owner + p.cursorOffset, p, module) : p.outputStart;
        const EntryState entry = makeEntry(p);
        const bool stackCovered = std::any_of(p.regions.begin(), p.regions.end(),
            [&entry](const Region& r) {
                return r.base + 8 <= entry.rsp && entry.rsp < r.base + r.size;
            });
        if (!stackCovered) throw std::runtime_error("captured_stack_not_mapped");
        const uint64_t literal = p.gsSites.empty() ? 0 :
            nearLiteral(module.base, module.size);
        patchCode(p, module, literal);
        void* handler = AddVectoredExceptionHandler(1, exceptionHandler);
        if (!handler) throw std::runtime_error("exception_handler_install_failed");
        LARGE_INTEGER frequency{}, begin{}, end{};
        if (!QueryPerformanceFrequency(&frequency) || !frequency.QuadPart)
            throw std::runtime_error("QPC_frequency_failed");
        std::vector<double> samples;
        for (uint32_t run = 0; run < repeats; ++run) {
            if (run) {
                resetRegions(p);                 // outside the timed interval
                if (p.version == 2) checkCapacity(p, module);
                else checkScatterOutput(p, module);
                patchCode(p, module, literal);   // restore private code patches
            }
            std::memset(&g_exitState, 0, sizeof(g_exitState));
            InterlockedExchange(&g_trapCode, 0);
            g_exceptionCode = 0;
            g_exceptionParameters = 0;
            g_exceptionInfo0 = g_exceptionInfo1 = 0;
            InterlockedExchange(&g_inReplay, 1);
            QueryPerformanceCounter(&begin);
            StartIsolated(&entry);
            QueryPerformanceCounter(&end);
            InterlockedExchange(&g_inReplay, 0);
            if (g_trapCode) return reportTrap(p, module, run, "restored");
            compareExitContext(p);
            const size_t mismatches = compareAfter(p, initialCursor);
            if (mismatches) {
                std::cerr << "after_snapshot_mismatch run=" << run <<
                    " bytes=" << mismatches << "\n";
                return 4;
            }
            samples.push_back(1000.0 *
                static_cast<double>(end.QuadPart - begin.QuadPart) /
                static_cast<double>(frequency.QuadPart));
        }
        const EffectAudit effects = auditCloneEffects(p, entry, initialCursor);
        const bool everyRecordEmitted = p.version == 3 || (
            p.expectedFinalCursor >= initialCursor &&
            p.expectedFinalCursor - initialCursor ==
            static_cast<uint64_t>(p.packetCount) * p.stride);
        const bool warmEligible = p.fullRecordZero && everyRecordEmitted &&
                                  effects.changedOutside == 0;
        std::vector<double> warmSamples;
        if (warmEligible) {
            for (uint32_t run = 0; run < warmRepeats; ++run) {
                resetWarmState(p, entry, initialCursor);
                if (p.version == 2) checkCapacity(p, module);
                else checkScatterOutput(p, module);
                std::memset(&g_exitState, 0, sizeof(g_exitState));
                InterlockedExchange(&g_trapCode, 0);
                g_exceptionCode = 0;
                g_exceptionParameters = 0;
                g_exceptionInfo0 = g_exceptionInfo1 = 0;
                InterlockedExchange(&g_inReplay, 1);
                QueryPerformanceCounter(&begin);
                StartIsolated(&entry);
                QueryPerformanceCounter(&end);
                InterlockedExchange(&g_inReplay, 0);
                if (g_trapCode) return reportTrap(p, module, run, "warm");
                compareExitContext(p);
                if (compareAfter(p, initialCursor))
                    throw std::runtime_error("warm_after_snapshot_mismatch");
                const EffectAudit warmEffects =
                    auditCloneEffects(p, entry, initialCursor);
                if (warmEffects.changedOutside)
                    throw std::runtime_error("warm_write_outside_audited_ranges");
                warmSamples.push_back(1000.0 *
                    static_cast<double>(end.QuadPart - begin.QuadPart) /
                    static_cast<double>(frequency.QuadPart));
            }
        }
        RemoveVectoredExceptionHandler(handler);
        auto sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        std::cout << "{\"original_cpu_loop_restored_median_ms\":" << median
                  << ",\"contract_kind\":\""
                  << (p.version == 3 ? "word_scatter" : "append")
                  << "\""
                  << ",\"packet_count\":" << p.packetCount
                  << ",\"after_snapshot_equal\":true"
                  << ",\"isolated_replay\":true"
                  << ",\"live_replacement_admitted\":false"
                  << ",\"restored_reset_mode\":\"full_views_before_each_run\""
                  << ",\"isolated_effect_audit_outside_bytes\":"
                  << effects.changedOutside
                  << ",\"isolated_effect_audit_first_outside_hex\":\"0x"
                  << std::hex << effects.firstOutside << std::dec << "\""
                  << ",\"warm_comparison_admitted\":"
                  << (warmEligible ? "true" : "false")
                  << ",\"writecombine_output_preserved\":"
                  << (p.version == 3 ? "true" : "false")
                  << ",\"restored_samples_ms\":[";
        for (size_t i = 0; i < samples.size(); ++i)
            std::cout << (i ? "," : "") << samples[i];
        std::cout << "],\"warm_samples_ms\":[";
        for (size_t i = 0; i < warmSamples.size(); ++i)
            std::cout << (i ? "," : "") << warmSamples[i];
        std::cout << "]";
        if (!warmSamples.empty()) {
            auto sortedWarm = warmSamples;
            std::sort(sortedWarm.begin(), sortedWarm.end());
            std::cout << ",\"original_cpu_loop_warm_median_ms\":"
                      << sortedWarm[sortedWarm.size() / 2];
        }
        std::cout << "}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "replay_refused: " << e.what() << "\n";
        return 2;
    }
}
