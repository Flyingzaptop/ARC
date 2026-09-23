#include "arc/cpu/region_model.hpp"
#include "arc/cpu/publication.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <new>

static std::size_t heap_allocations{};
void* operator new(std::size_t n) {
    ++heap_allocations;
    if (auto* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace arc::cpu;

static Region region() {
    Region r{};
    r.proven_no_external_effects = true;
    r.flags_dead_on_exit = true;
    r.live_out_mask = reg_bit(Reg::Rax) | reg_bit(Reg::Rdx);
    // RAX = (RCX + 5) * 8; RDX = (R8 ^ 7) + R9.
    r.instructions[0] = {Op::Lea, Reg::Rax, Operand::reg(Reg::Rcx), Operand::imm(5), 1, 0};
    r.instructions[1] = {Op::Imul, Reg::Rax, Operand::reg(Reg::Rax), Operand::imm(8)};
    r.instructions[2] = {Op::Xor, Reg::Rdx, Operand::reg(Reg::R8), Operand::imm(7)};
    r.instructions[3] = {Op::Add, Reg::Rdx, Operand::reg(Reg::Rdx), Operand::reg(Reg::R9)};
    r.count = 4;
    return r;
}

static State state(std::uint64_t rcx, std::uint64_t r8, std::uint64_t r9) {
    State s{};
    s.regs[static_cast<unsigned>(Reg::Rcx)] = rcx;
    s.regs[static_cast<unsigned>(Reg::R8)] = r8;
    s.regs[static_cast<unsigned>(Reg::R9)] = r9;
    s.regs[static_cast<unsigned>(Reg::Rbx)] = 0xABCDEF;
    s.rflags = 0x202;
    return s;
}

static void same_outputs(const RunResult& a, const RunResult& b) {
    assert(a.output_mask == b.output_mask);
    for (unsigned r = 0; r < register_count; ++r)
        if (a.output_mask & (1u << r)) assert(a.state.regs[r] == b.state.regs[r]);
}

int main() {
    const auto original = region();
    const auto analysis = analyze(original);
    assert(analysis.accepted());
    assert(analysis.live_in_mask == (reg_bit(Reg::Rcx) | reg_bit(Reg::R8) | reg_bit(Reg::R9)));
    assert(analysis.nodes[1].live_in_dependencies == reg_bit(Reg::Rcx));
    assert(analysis.nodes[3].live_in_dependencies == (reg_bit(Reg::R8) | reg_bit(Reg::R9)));

    const auto s1 = state(11, 21, 3);
    auto oracle = execute(analysis, s1);
    assert(oracle.state.regs[static_cast<unsigned>(Reg::Rax)] == 128);
    assert(oracle.state.regs[static_cast<unsigned>(Reg::Rdx)] == ((21 ^ 7) + 3));
    assert(!oracle.flags_valid && oracle.evaluated_nodes == 4);

    const auto spec = specialize(analysis, s1, reg_bit(Reg::Rcx), 99);
    assert(spec.valid && spec.folded_nodes == 0b0011);
    bool hit{};
    auto specialized = execute_specialized(analysis, spec, s1, 99, hit);
    assert(hit && specialized.reused_nodes == 2 && specialized.evaluated_nodes == 2);
    same_outputs(oracle, specialized);
    const auto s2 = state(12, 21, 3);
    specialized = execute_specialized(analysis, spec, s2, 99, hit);
    assert(!hit && specialized.evaluated_nodes == 4);
    same_outputs(execute(analysis, s2), specialized);
    specialized = execute_specialized(analysis, spec, s1, 100, hit);
    assert(!hit && specialized.evaluated_nodes == 4);
    auto altered = original;
    altered.instructions[1].b = Operand::imm(9);
    const auto different_analysis = analyze(altered);
    specialized = execute_specialized(different_analysis, spec, s1, 99, hit);
    assert(!hit && specialized.evaluated_nodes == 4);
    same_outputs(execute(different_analysis, s1), specialized);

    RegionCache cache(analysis);
    auto first = cache.run(RegionCache::Action::Memo, s1, 1);
    auto second = cache.run(RegionCache::Action::Memo, s1, 1);
    same_outputs(first, second);
    assert(first.reused_nodes == 0 && second.reused_nodes == analysis.count);
    assert(cache.counters().hits == 1 && cache.counters().misses == 1);
    assert(cache.counters().restored_registers == 2);
    // Exact key comparison includes both apparently stable and changed inputs.
    auto changed = cache.run(RegionCache::Action::Memo, state(11, 21, 4), 1);
    same_outputs(execute(analysis, state(11, 21, 4)), changed);
    assert(cache.counters().misses == 2);
    auto generation_changed = cache.run(RegionCache::Action::Memo, s1, 2);
    same_outputs(first, generation_changed);
    assert(cache.counters().misses == 3);

    RegionCache incremental(analysis);
    auto baseline = incremental.run(RegionCache::Action::Incremental, s1, 7);
    assert(baseline.evaluated_nodes == 4);
    auto only_r9 = incremental.run(RegionCache::Action::Incremental, state(11, 21, 9), 7);
    same_outputs(execute(analysis, state(11, 21, 9)), only_r9);
    assert(only_r9.evaluated_nodes == 1 && only_r9.reused_nodes == 3);
    auto only_rcx = incremental.run(RegionCache::Action::Incremental, state(13, 21, 9), 7);
    same_outputs(execute(analysis, state(13, 21, 9)), only_rcx);
    assert(only_rcx.evaluated_nodes == 2 && only_rcx.reused_nodes == 2);
    assert(incremental.counters().dirty_nodes == 3);
    auto expired = incremental.run(RegionCache::Action::Incremental, s1, 8);
    assert(expired.evaluated_nodes == 4);

    // A memory access, call, branch, hidden state or live flag effect must be
    // rejected by the decoder. This tests the admission boundary it reports.
    auto unsafe = original;
    unsafe.proven_no_external_effects = false;
    assert(analyze(unsafe).rejection == Reject::ExternalEffects);
    unsafe = original;
    unsafe.flags_dead_on_exit = false;
    assert(analyze(unsafe).rejection == Reject::FlagsLive);
    unsafe = original;
    unsafe.instructions[0].dst = Reg::Rsp;
    assert(analyze(unsafe).rejection == Reject::StackPointerWrite);

    auto mov = original;
    mov.count = 1;
    mov.instructions[0] = {Op::Mov, Reg::Rax, Operand::imm(0xF00D)};
    mov.live_out_mask = reg_bit(Reg::Rax);
    mov.flags_dead_on_exit = false;
    const auto mov_result = execute(analyze(mov), s1);
    assert(mov_result.flags_valid && mov_result.state.rflags == s1.rflags);
    const auto unconditional = specialize(analyze(mov), s1, 0, 12);
    assert(unconditional.valid && unconditional.folded_nodes == 1);
    auto unconditional_result = execute_specialized(analyze(mov), unconditional, s2, 12, hit);
    assert(hit && unconditional_result.reused_nodes == 1);
    same_outputs(mov_result, unconditional_result);

    const ProfitEstimate expensive{100, .8, 100, 8, 3, 50, 100};
    const ProfitEstimate cheap{100, .01, 8, 8, 3, 50, 100};
    assert(expensive.profitable() && !cheap.profitable());

    CpuPublicationInput input{};
    input.id = 1;
    input.code_begin = input.original_entry = 0x1000;
    input.code_end = 0x1010;
    input.variant_entry = 0x2000;
    input.fingerprint = {0x1234, 7, 1, 0, 0, 1};
    input.kind = arc::CandidateKind::ExactReuse;
    auto record = make_cpu_candidate_record(analysis, input);
    auto admitted = arc::admit_candidate(record);
    assert(admitted.accepted() && admitted.projection.backend == arc::CandidateBackend::Cpu);
    const auto before_admission = heap_allocations;
    const auto pod_admission = make_cpu_pod_admission(analysis, input);
    assert(heap_allocations == before_admission);
    assert(pod_admission.accepted() && pod_admission.projection.kind == admitted.projection.kind);
    assert(record.supported_model.empty() && record.assumptions.empty());
    arc::PublishedCandidate publication;
    assert(publish_cpu_candidate(publication, pod_admission, true));
    {
        auto lease = publication.try_acquire(input.fingerprint, input.required_guards);
        assert(lease && lease->kind == arc::CandidateKind::ExactReuse);
        assert(!publication.try_acquire(input.fingerprint, 0));
        publication.invalidate(arc::CandidateReason::GenerationChanged);
        assert(!publication.try_acquire(input.fingerprint, input.required_guards));
        assert(publication.outstanding() == 1);
    }
    assert(publication.outstanding() == 0);
    auto unsafe_record = make_cpu_candidate_record(analyze(unsafe), input);
    assert(!arc::admit_candidate(unsafe_record).accepted());
}
