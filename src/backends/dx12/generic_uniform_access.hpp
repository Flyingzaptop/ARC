#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace arc::dx12::shader {
struct UniformWords {std::array<std::uint32_t,4> words{};unsigned valid_mask{};bool operator==(const UniformWords&)const=default;};
using UniformReader=std::function<UniformWords(unsigned range_id,unsigned shader_register,unsigned byte_offset)>;
// Optional motion-state telemetry, never an admission proof. Only literal
// float CBV loads with statically known handles; no names/layout allowlists.
std::vector<std::array<unsigned,3>> floating_uniform_reads(std::string_view);
struct UsedRange {bool all{};std::set<unsigned> indices;};
struct ResourceUsage {
    bool complete{};
    std::string reason;
    unsigned steps{},states{};
    std::map<std::pair<unsigned,unsigned>,UsedRange> ranges;
};
class UniformAccessProgram {
public:
    struct Impl;
    static std::shared_ptr<const UniformAccessProgram> compile(std::string_view ir);
    // Abstract execution: pixel-dependent values are unknown and both branches
    // are explored. Only integer CBV data supplied by the caller is concrete.
    // The caller must provide a stable submission-time snapshot of CPU-visible
    // constant data; GPU-written data remains unknown, never guessed.
    [[nodiscard]] ResourceUsage evaluate(const UniformReader&,unsigned step_budget=32768)const;
private:
    std::shared_ptr<const Impl> impl_;
};
}
