#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// A deliberately closed, register-only x64 region model. The decoder must
// prove the boundary facts before constructing a Region: no branch, memory,
// call, faulting instruction, or observable flag effect may be hidden in it.
namespace arc::cpu {

enum class Reg : std::uint8_t {
    Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi,
    R8, R9, R10, R11, R12, R13, R14, R15
};
constexpr std::size_t register_count = 16;
constexpr std::size_t max_instructions = 64;
constexpr std::uint16_t reg_bit(Reg r) noexcept {
    return static_cast<std::uint16_t>(1u << static_cast<unsigned>(r));
}
constexpr bool valid_reg(Reg r) noexcept { return static_cast<unsigned>(r) < register_count; }

enum class Op : std::uint8_t { Mov, Lea, Add, Sub, Xor, And, Or, Imul };
struct Operand {
    bool is_register{};
    Reg register_id{Reg::Rax};
    std::uint64_t immediate{};
    static constexpr Operand reg(Reg r) noexcept { return {true, r, 0}; }
    static constexpr Operand imm(std::uint64_t n) noexcept { return {false, Reg::Rax, n}; }
};
struct Instruction {
    Op op{Op::Mov};
    Reg dst{Reg::Rax};
    Operand a{}, b{};
    std::uint8_t scale{1}; // LEA only: a + b * scale + displacement
    std::int64_t displacement{};
};
struct Region {
    std::array<Instruction, max_instructions> instructions{};
    std::size_t count{};
    std::uint16_t live_out_mask{};
    bool flags_dead_on_exit{};
    bool proven_no_external_effects{};
};
struct State {
    std::array<std::uint64_t, register_count> regs{};
    std::uint64_t rflags{};
};
enum class Reject : std::uint8_t {
    None, EmptyOrOversize, ExternalEffects, InvalidInstruction,
    StackPointerWrite, FlagsLive, InvalidLiveOut
};
struct Node {
    Instruction instruction{};
    std::uint16_t live_in_dependencies{};
    int a_parent{-1}, b_parent{-1};
};
struct Analysis {
    std::array<Node, max_instructions> nodes{};
    std::size_t count{};
    std::uint16_t live_in_mask{}, written_mask{}, live_out_mask{};
    bool flags_dead_on_exit{};
    Reject rejection{Reject::EmptyOrOversize};
    [[nodiscard]] bool accepted() const noexcept { return rejection == Reject::None; }
};
constexpr bool reads_b(Op op) noexcept { return op != Op::Mov; }
constexpr bool changes_flags(Op op) noexcept { return op != Op::Mov && op != Op::Lea; }
constexpr bool valid_op(Op op) noexcept { return static_cast<unsigned>(op) <= static_cast<unsigned>(Op::Imul); }
constexpr bool valid_scale(std::uint8_t scale) noexcept {
    return scale == 1 || scale == 2 || scale == 4 || scale == 8;
}
[[nodiscard]] inline Analysis analyze(const Region& region) noexcept {
    Analysis out{};
    if (!region.count || region.count > max_instructions) return out;
    if (!region.proven_no_external_effects) { out.rejection = Reject::ExternalEffects; return out; }
    if (region.live_out_mask & reg_bit(Reg::Rsp)) { out.rejection = Reject::InvalidLiveOut; return out; }
    std::array<int, register_count> latest{};
    latest.fill(-1);
    for (std::size_t i = 0; i < region.count; ++i) {
        const auto& ins = region.instructions[i];
        if (!valid_op(ins.op) || !valid_reg(ins.dst) ||
            (ins.a.is_register && !valid_reg(ins.a.register_id)) ||
            (reads_b(ins.op) && ins.b.is_register && !valid_reg(ins.b.register_id)) ||
            (ins.op == Op::Lea && !valid_scale(ins.scale)) ||
            (ins.op != Op::Lea && (ins.scale != 1 || ins.displacement != 0))) {
            out.rejection = Reject::InvalidInstruction; return out;
        }
        if (ins.dst == Reg::Rsp) { out.rejection = Reject::StackPointerWrite; return out; }
        if (changes_flags(ins.op) && !region.flags_dead_on_exit) {
            out.rejection = Reject::FlagsLive; return out;
        }
        Node node{};
        node.instruction = ins;
        auto add_source = [&](Operand src, int& parent) {
            if (!src.is_register) return;
            const auto index = static_cast<unsigned>(src.register_id);
            parent = latest[index];
            if (parent >= 0) node.live_in_dependencies |= out.nodes[parent].live_in_dependencies;
            else node.live_in_dependencies |= reg_bit(src.register_id);
        };
        add_source(ins.a, node.a_parent);
        if (reads_b(ins.op)) add_source(ins.b, node.b_parent);
        out.nodes[i] = node;
        out.live_in_mask |= node.live_in_dependencies;
        out.written_mask |= reg_bit(ins.dst);
        latest[static_cast<unsigned>(ins.dst)] = static_cast<int>(i);
    }
    if (region.live_out_mask & ~out.written_mask) {
        out.rejection = Reject::InvalidLiveOut; return out;
    }
    out.count = region.count;
    out.live_out_mask = region.live_out_mask;
    out.flags_dead_on_exit = region.flags_dead_on_exit;
    out.rejection = Reject::None;
    return out;
}

constexpr std::uint64_t calculate(const Instruction& ins, std::uint64_t a,
                                  std::uint64_t b) noexcept {
    switch (ins.op) {
    case Op::Mov: return a;
    case Op::Lea: return a + b * ins.scale + static_cast<std::uint64_t>(ins.displacement);
    case Op::Add: return a + b;
    case Op::Sub: return a - b;
    case Op::Xor: return a ^ b;
    case Op::And: return a & b;
    case Op::Or: return a | b;
    case Op::Imul: return a * b; // low 64 bits of two-operand IMUL
    }
    return 0;
}
struct RunResult {
    State state{};
    std::uint16_t output_mask{};
    bool flags_valid{};
    std::uint32_t evaluated_nodes{}, reused_nodes{};
};
[[nodiscard]] inline RunResult execute(const Analysis& analysis, const State& input) noexcept {
    RunResult result{};
    if (!analysis.accepted()) return result;
    result.state = input;
    result.output_mask = analysis.live_out_mask;
    result.flags_valid = !analysis.flags_dead_on_exit;
    for (std::size_t i = 0; i < analysis.count; ++i) {
        const auto& ins = analysis.nodes[i].instruction;
        const auto value = [&](Operand src) {
            return src.is_register ? result.state.regs[static_cast<unsigned>(src.register_id)] : src.immediate;
        };
        result.state.regs[static_cast<unsigned>(ins.dst)] = calculate(ins, value(ins.a), value(ins.b));
        ++result.evaluated_nodes;
    }
    return result;
}

// The guard is checked against the current complete input state before any
// output is produced. Constant nodes are derived from dependency masks, not
// caller supplied hints. A mismatched guard executes the original model.
struct Specialization {
    std::uint64_t generation{};
    std::uint16_t guard_mask{};
    std::array<std::uint64_t, register_count> guard_values{};
    std::array<std::uint64_t, max_instructions> folded_values{};
    std::array<Instruction, max_instructions> original_instructions{};
    std::size_t original_count{};
    std::uint16_t original_live_in_mask{}, original_live_out_mask{};
    bool original_flags_dead{};
    std::uint64_t folded_nodes{};
    bool valid{};
};
constexpr bool same_operand(const Operand& a, const Operand& b) noexcept {
    return a.is_register == b.is_register &&
           (a.is_register ? a.register_id == b.register_id : a.immediate == b.immediate);
}
constexpr bool same_instruction(const Instruction& a, const Instruction& b) noexcept {
    return a.op == b.op && a.dst == b.dst && same_operand(a.a, b.a) &&
           same_operand(a.b, b.b) && a.scale == b.scale && a.displacement == b.displacement;
}
[[nodiscard]] inline bool matches_analysis(const Specialization& spec, const Analysis& analysis,
                                           std::uint64_t generation) noexcept {
    if (!spec.valid || !analysis.accepted() || !generation || spec.generation != generation ||
        spec.original_count != analysis.count || spec.original_live_in_mask != analysis.live_in_mask ||
        spec.original_live_out_mask != analysis.live_out_mask ||
        spec.original_flags_dead != analysis.flags_dead_on_exit) return false;
    for (std::size_t i = 0; i < analysis.count; ++i)
        if (!same_instruction(spec.original_instructions[i], analysis.nodes[i].instruction)) return false;
    return true;
}
[[nodiscard]] inline Specialization specialize(const Analysis& analysis, const State& observed,
                                               std::uint16_t guard_mask,
                                               std::uint64_t generation) noexcept {
    Specialization spec{};
    if (!analysis.accepted() || !generation ||
        (guard_mask & ~analysis.live_in_mask)) return spec;
    spec.generation = generation;
    spec.guard_mask = guard_mask;
    spec.guard_values = observed.regs;
    spec.original_count = analysis.count;
    spec.original_live_in_mask = analysis.live_in_mask;
    spec.original_live_out_mask = analysis.live_out_mask;
    spec.original_flags_dead = analysis.flags_dead_on_exit;
    State state = observed;
    for (std::size_t i = 0; i < analysis.count; ++i) {
        const auto& node = analysis.nodes[i];
        spec.original_instructions[i] = node.instruction;
        const auto value = [&](Operand src) {
            return src.is_register ? state.regs[static_cast<unsigned>(src.register_id)] : src.immediate;
        };
        const auto v = calculate(node.instruction, value(node.instruction.a), value(node.instruction.b));
        state.regs[static_cast<unsigned>(node.instruction.dst)] = v;
        if (!(node.live_in_dependencies & ~guard_mask)) {
            spec.folded_nodes |= std::uint64_t{1} << i;
            spec.folded_values[i] = v;
        }
    }
    spec.valid = spec.folded_nodes != 0;
    return spec;
}
[[nodiscard]] inline RunResult execute_specialized(const Analysis& analysis, const Specialization& spec,
                                                   const State& input, std::uint64_t generation,
                                                   bool& guard_hit) noexcept {
    guard_hit = false;
    if (!analysis.accepted()) return {};
    if (!matches_analysis(spec, analysis, generation)) return execute(analysis, input);
    for (unsigned r = 0; r < register_count; ++r) {
        if ((spec.guard_mask & (1u << r)) && input.regs[r] != spec.guard_values[r])
            return execute(analysis, input);
    }
    guard_hit = true;
    RunResult result{};
    result.state = input;
    result.output_mask = analysis.live_out_mask;
    result.flags_valid = !analysis.flags_dead_on_exit;
    for (std::size_t i = 0; i < analysis.count; ++i) {
        const auto& ins = analysis.nodes[i].instruction;
        if (spec.folded_nodes & (std::uint64_t{1} << i)) {
            result.state.regs[static_cast<unsigned>(ins.dst)] = spec.folded_values[i];
            ++result.reused_nodes;
        } else {
            const auto value = [&](Operand src) {
                return src.is_register ? result.state.regs[static_cast<unsigned>(src.register_id)] : src.immediate;
            };
            result.state.regs[static_cast<unsigned>(ins.dst)] = calculate(ins, value(ins.a), value(ins.b));
            ++result.evaluated_nodes;
        }
    }
    return result;
}

// Each instance belongs to one calling thread. Generation represents code,
// module lifetime and dependency epoch and must be supplied by the owner.
class RegionCache {
public:
    static constexpr std::size_t entries = 4;
    enum class Action : std::uint8_t { Original, Memo, Incremental };
    struct Counters {
        std::uint64_t calls{}, hits{}, misses{}, dirty_nodes{}, fallbacks{},
                      key_register_compares{}, restored_registers{}, generation_invalidations{};
    };
private:
    struct Entry {
        bool valid{};
        std::array<std::uint64_t, register_count> key{};
        RunResult result{};
    };
    Analysis analysis_{};
    std::uint64_t generation_{};
    std::array<Entry, entries> memo_{};
    std::size_t next_memo_{};
    bool incremental_valid_{};
    std::array<std::uint64_t, register_count> incremental_key_{};
    std::array<std::uint64_t, max_instructions> node_values_{};
    Counters counters_{};
    [[nodiscard]] bool same_inputs(const std::array<std::uint64_t, register_count>& key,
                                   const State& input) noexcept {
        for (unsigned r = 0; r < register_count; ++r) {
            if (analysis_.live_in_mask & (1u << r)) {
                ++counters_.key_register_compares;
                if (key[r] != input.regs[r]) return false;
            }
        }
        return true;
    }
    static void restore(RunResult& result, const RunResult& cached, std::uint16_t mask) noexcept {
        for (unsigned r = 0; r < register_count; ++r)
            if (mask & (1u << r)) result.state.regs[r] = cached.state.regs[r];
    }
public:
    explicit RegionCache(Analysis analysis) noexcept : analysis_(analysis) {}
    void set_generation(std::uint64_t generation) noexcept {
        if (generation_ == generation) return;
        if (generation_) ++counters_.generation_invalidations;
        generation_ = generation;
        for (auto& e : memo_) e.valid = false;
        incremental_valid_ = false;
    }
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }
    [[nodiscard]] RunResult run(Action action, const State& input, std::uint64_t generation) noexcept {
        ++counters_.calls;
        if (!analysis_.accepted() || !generation) {
            ++counters_.fallbacks;
            return {};
        }
        set_generation(generation);
        if (action == Action::Original) return execute(analysis_, input);
        if (action != Action::Memo && action != Action::Incremental) {
            ++counters_.fallbacks;
            return execute(analysis_, input);
        }
        if (action == Action::Memo) {
            for (const auto& entry : memo_) {
                if (!entry.valid || !same_inputs(entry.key, input)) continue;
                ++counters_.hits;
                RunResult result{input, analysis_.live_out_mask, !analysis_.flags_dead_on_exit,
                                 0, static_cast<std::uint32_t>(analysis_.count)};
                restore(result, entry.result, analysis_.live_out_mask);
                for (unsigned r = 0; r < register_count; ++r)
                    if (analysis_.live_out_mask & (1u << r)) ++counters_.restored_registers;
                return result;
            }
            ++counters_.misses;
            auto result = execute(analysis_, input);
            auto& entry = memo_[next_memo_++ % entries];
            entry = {true, input.regs, result};
            return result;
        }
        if (!incremental_valid_) {
            ++counters_.misses;
            RunResult result{input, analysis_.live_out_mask, !analysis_.flags_dead_on_exit, 0, 0};
            for (std::size_t i = 0; i < analysis_.count; ++i) {
                const auto& ins = analysis_.nodes[i].instruction;
                const auto value = [&](Operand src) {
                    return src.is_register ? result.state.regs[static_cast<unsigned>(src.register_id)] : src.immediate;
                };
                node_values_[i] = calculate(ins, value(ins.a), value(ins.b));
                result.state.regs[static_cast<unsigned>(ins.dst)] = node_values_[i];
                ++result.evaluated_nodes;
            }
            incremental_key_ = input.regs;
            incremental_valid_ = true;
            return result;
        }
        std::uint16_t changed{};
        for (unsigned r = 0; r < register_count; ++r) {
            if (!(analysis_.live_in_mask & (1u << r))) continue;
            ++counters_.key_register_compares;
            if (incremental_key_[r] != input.regs[r]) changed |= static_cast<std::uint16_t>(1u << r);
        }
        RunResult result{input, analysis_.live_out_mask, !analysis_.flags_dead_on_exit, 0, 0};
        for (std::size_t i = 0; i < analysis_.count; ++i) {
            const auto& node = analysis_.nodes[i];
            const auto& ins = node.instruction;
            std::uint64_t v;
            if (node.live_in_dependencies & changed) {
                const auto value = [&](Operand src) {
                    return src.is_register ? result.state.regs[static_cast<unsigned>(src.register_id)] : src.immediate;
                };
                v = calculate(ins, value(ins.a), value(ins.b));
                node_values_[i] = v;
                ++result.evaluated_nodes;
                ++counters_.dirty_nodes;
            } else {
                v = node_values_[i];
                ++result.reused_nodes;
            }
            result.state.regs[static_cast<unsigned>(ins.dst)] = v;
        }
        incremental_key_ = input.regs;
        if (!changed) ++counters_.hits;
        else ++counters_.misses;
        return result;
    }
};

// Work model deliberately includes guard/key/restoration and analysis costs.
struct ProfitEstimate {
    double calls{}, hit_rate{}, original_cost_ns{}, restore_cost_ns{}, guard_cost_ns{},
           tracking_cost_ns{}, amortized_analysis_cost_ns{};
    [[nodiscard]] double net_work_saved_ns() const noexcept {
        return calls * hit_rate * (original_cost_ns - restore_cost_ns) -
               calls * guard_cost_ns - tracking_cost_ns - amortized_analysis_cost_ns;
    }
    [[nodiscard]] bool profitable() const noexcept { return net_work_saved_ns() > 0; }
};

} // namespace arc::cpu
