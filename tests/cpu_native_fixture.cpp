#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <xmmintrin.h>

extern "C" std::uint64_t arc_native_region(std::uint64_t a,std::uint64_t b);
extern "C" std::uint64_t arc_native_side_effect(std::uint64_t a,std::uint64_t* out);
#include "cpu_late_regions.hpp"
struct NativeProbe {
    std::uint64_t rax{}, r10{}, flags_before{}, flags_after{},
                  rsp_before{}, rsp_after{};
    std::uint32_t mxcsr_before{}, mxcsr_after{};
    std::array<std::uint8_t, 16> xmm0_after{}, xmm1_after{};
    std::uint64_t rbx_before{}, rbx_after{}, r12_before{}, r12_after{};
    std::uint64_t rcx_after{}, rdx_after{}, x87_value_bits{};
    std::uint16_t x87_control_before{}, x87_control_after{};
};
static_assert(sizeof(NativeProbe) == 152);
static_assert(offsetof(NativeProbe, xmm0_after) == 56);
static_assert(offsetof(NativeProbe, x87_value_bits) == 136);
extern "C" void arc_native_state_probe(std::uint64_t a,std::uint64_t b,NativeProbe* out);

static constexpr std::array<std::uint8_t,16> xmm0_expected{
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
    0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
static constexpr std::array<std::uint8_t,16> xmm1_expected{
    0xff,0xee,0xdd,0xcc,0xbb,0xaa,0x99,0x88,
    0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x00};
static constexpr std::uint64_t arithmetic_flags = 0x8d5; // CF, PF, AF, ZF, SF, OF

static bool transition(const char* action,const char* path) {
    if (!action) return true;
    if (std::strcmp(action,"--stop")==0) {
        const HANDLE file=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file==INVALID_HANDLE_VALUE) return false;
        const HANDLE mapping=CreateFileMappingA(file,nullptr,PAGE_READWRITE,0,0,nullptr);
        if (!mapping) { CloseHandle(file); return false; }
        void* view=MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,sizeof(LONG));
        if (!view) { CloseHandle(mapping); CloseHandle(file); return false; }
        InterlockedExchange(static_cast<volatile LONG*>(view),1);
        const bool flushed=FlushViewOfFile(view,sizeof(LONG))!=0;
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        CloseHandle(file);
        return flushed;
    }
    if (std::strcmp(action,"--protect-other")==0) {
        void* const page=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if (!page) return false;
        DWORD old{};
        const bool protected_page=VirtualProtect(page,4096,PAGE_READONLY,&old)!=0;
        VirtualFree(page,0,MEM_RELEASE);
        return protected_page;
    }
    DWORD old_protection{};
    void* const page=reinterpret_cast<void*>(&arc_native_region);
    if(std::strcmp(action,"--protect-same")==0)
        return VirtualProtect(page,1,PAGE_EXECUTE_READ,&old_protection)!=0;
    if (!VirtualProtect(page,1,PAGE_EXECUTE_READWRITE,&old_protection)) return false;
    DWORD ignored{};
    return VirtualProtect(page,1,old_protection,&ignored)!=0;
}

int main(int argc,char** argv) {
    const char* action=nullptr;
    const char* path=nullptr;
    if (argc==3 && std::strcmp(argv[1],"--stop")==0) {
        action=argv[1]; path=argv[2];
    } else if (argc==2 && (std::strcmp(argv[1],"--protect")==0 ||
                           std::strcmp(argv[1],"--protect-other")==0 ||
                           std::strcmp(argv[1],"--protect-same")==0 ||
                           std::strcmp(argv[1],"--fp-state")==0 ||
                           std::strcmp(argv[1],"--study-two")==0 ||
                           std::strcmp(argv[1],"--cost-churn")==0 ||
                           std::strcmp(argv[1],"--late-hot")==0 || std::strcmp(argv[1],"--return-evicted")==0)) {
        action=argv[1];
    } else if (argc!=1) {
        std::fprintf(stderr,"usage: cpu_native_fixture [--stop path | --protect | --protect-other | --protect-same | --late-hot | --return-evicted]\n");
        return 3;
    }
    const auto started=GetTickCount64();
    const auto saved_mxcsr=_mm_getcsr();
    if(action && std::strcmp(action,"--fp-state")==0) _mm_setcsr(0x0f80); // precision exceptions unmasked
    if(action && std::strcmp(action,"--cost-churn")==0)
        for(unsigned i=0;i<128;++i) for(unsigned n=0;n<5;++n)
            if(late_regions[n](41)!=42) return 12;
    if (action && std::strcmp(action,"--late-hot")==0) {
        for (const auto fn:late_regions)
            if (fn(41)!=42) return 6;
        for (unsigned i=0;i<256;++i)
            if (late_regions[64](41)!=42) return 7;
        for (unsigned i=0;i<256;++i)
            if (late_regions[209](41)!=42) return 8;
    }
    if(action && std::strcmp(action,"--return-evicted")==0) {
        for(const auto fn:late_regions) if(fn(41)!=42) return 9;
        for(unsigned region=64;region<210;++region)
            for(unsigned n=0;n<64;++n) if(late_regions[region](41)!=42) return 10;
        for(unsigned n=0;n<256;++n) if(late_regions[0](41)!=42) return 11;
        std::printf("revisited_offset=%llu\n",(unsigned long long)(reinterpret_cast<std::uintptr_t>(late_regions[0])-reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))));
    }
    std::uint64_t side=0;
    for (unsigned i=0;i<512;++i) {
        if(i==256 && action && std::strcmp(action,"--study-two")==0) Sleep(1200);
        if (i==256 && (!action || (std::strcmp(action,"--late-hot")!=0 && std::strcmp(action,"--return-evicted")!=0 && std::strcmp(action,"--fp-state")!=0 && std::strcmp(action,"--study-two")!=0 && std::strcmp(action,"--cost-churn")!=0)) && !transition(action,path)) {
            std::fprintf(stderr,"transition failed: %s\n",action);
            return 4;
        }
        const auto a=(i%7==0)?13ull:11ull;
        const auto b=(i%9==0)?5ull:3ull;
        const auto expected=a+2*b+25;
        const auto actual=arc_native_region(a,b);
        if (actual!=expected) { std::fprintf(stderr,"region mismatch %u: %llu != %llu\n",i,
            (unsigned long long)actual,(unsigned long long)expected); return 1; }
        NativeProbe probe{};
        arc_native_state_probe(a,b,&probe);
        if (probe.rax!=expected || probe.r10!=b+9 ||
            (probe.flags_before & arithmetic_flags)!=(probe.flags_after & arithmetic_flags) ||
            probe.rsp_before!=probe.rsp_after || probe.mxcsr_before!=probe.mxcsr_after ||
            probe.xmm0_after!=xmm0_expected || probe.xmm1_after!=xmm1_expected ||
            probe.rbx_before!=probe.rbx_after || probe.r12_before!=probe.r12_after ||
            probe.rcx_after!=a || probe.rdx_after!=b ||
            probe.x87_value_bits!=0x3ff0000000000000ull ||
            probe.x87_control_before!=probe.x87_control_after) {
            std::fprintf(stderr,"state mismatch %u: rax=%llu r10=%llu flags=%llx/%llx rsp=%llx/%llx mxcsr=%x/%x x87=%llx cw=%x/%x xmm0=%d xmm1=%d rb=%d r12=%d rcx=%d rdx=%d\n",
                i,(unsigned long long)probe.rax,(unsigned long long)probe.r10,
                (unsigned long long)probe.flags_before,(unsigned long long)probe.flags_after,
                (unsigned long long)probe.rsp_before,(unsigned long long)probe.rsp_after,
                probe.mxcsr_before,probe.mxcsr_after,(unsigned long long)probe.x87_value_bits,
                probe.x87_control_before,probe.x87_control_after,
                probe.xmm0_after==xmm0_expected,probe.xmm1_after==xmm1_expected,
                probe.rbx_before==probe.rbx_after,probe.r12_before==probe.r12_after,
                probe.rcx_after==a,probe.rdx_after==b);
            return 5;
        }
        side=~a;
        if (arc_native_side_effect(a,&side)!=a || side!=a) {
            std::fprintf(stderr,"side effect mismatch %u\n",i); return 2;
        }
    }
    _mm_setcsr(saved_mxcsr);
    std::printf("cpu-native-fixture: passed 512 iterations, GPR/flags/SIMD/MXCSR/x87/stack oracle, side effects; action=%s; elapsed_ms=%llu\n",
        action?action:"none",(unsigned long long)(GetTickCount64()-started));
    return 0;
}
