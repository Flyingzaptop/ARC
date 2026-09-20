#pragma once
#include <windows.h>
#include <iosfwd>
#include <cstdint>

namespace arc::dx12::placement {
// normal: original settings; prefer: ideal processor hint; core: ARC CPU sets;
// partition: additionally exclude the worker core from game DEFAULT CPU sets.
// Partitioning is opt-in, reversible, and is not exclusive OS core reservation.
bool initialize()noexcept;
bool configure(const wchar_t*)noexcept;
void snapshot(std::ostream&);
void present(void* swapchain)noexcept;
bool adaptive()noexcept;
void collect(std::uint64_t workload_epoch,std::uint64_t pipeline)noexcept; // background only
class Lease {
    unsigned slot_{32};
public:
    explicit Lease(HANDLE process=nullptr,HANDLE primary_thread=nullptr)noexcept;
    ~Lease();
    Lease(const Lease&)=delete;
    Lease& operator=(const Lease&)=delete;
};
}
