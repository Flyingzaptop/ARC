#pragma once
#include <windows.h>
#include <array>
#include <cstdint>

namespace arc::dx12::cpu_cost {
enum class Kind : unsigned {Thread,Compiler,Critic};
struct Snapshot {std::array<std::uint64_t,3> nanoseconds{};std::array<unsigned,3> live{};std::uint64_t failures{};};
// Register each ARC-owned thread once, and each child before waiting for it.
// Snapshots include LIVE child CPU usage, not just a delayed exit-time total.
class Registration {
    unsigned slot_{16};
    bool thread_{};
public:
    explicit Registration(Kind kind=Kind::Thread,HANDLE process=nullptr)noexcept;
    ~Registration();
    Registration(const Registration&)=delete;
    Registration& operator=(const Registration&)=delete;
};
Snapshot snapshot()noexcept;
bool on_worker_thread()noexcept;
}
