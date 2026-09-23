#pragma once
#include "dr_api.h"
#include "arc/cpu/region_model.hpp"

namespace arc::cpu::dbi {
struct Decoded {
    Region region{};
    app_pc start{}, end{};
    unsigned app_count{};
    std::uint64_t code_hash{};
    std::array<byte,1024> original_bytes{};
    std::size_t byte_count{};
};

inline int reg_index(reg_id_t r) {
    switch (r) {
    case DR_REG_RAX: return 0; case DR_REG_RCX: return 1;
    case DR_REG_RDX: return 2; case DR_REG_RBX: return 3;
    case DR_REG_RSP: return 4; case DR_REG_RBP: return 5;
    case DR_REG_RSI: return 6; case DR_REG_RDI: return 7;
    case DR_REG_R8: return 8; case DR_REG_R9: return 9;
    case DR_REG_R10: return 10; case DR_REG_R11: return 11;
    case DR_REG_R12: return 12; case DR_REG_R13: return 13;
    case DR_REG_R14: return 14; case DR_REG_R15: return 15;
    default: return -1;
    }
}
inline bool source(opnd_t src, Operand& out) {
    if (opnd_is_reg(src)) {
        const int n = reg_index(opnd_get_reg(src));
        if (n < 0) return false;
        out = Operand::reg(static_cast<Reg>(n));
        return true;
    }
    if (opnd_is_immed_int(src) && opnd_get_size(src) == OPSZ_8) {
        out = Operand::imm(static_cast<std::uint64_t>(opnd_get_immed_int(src)));
        return true;
    }
    return false;
}
inline bool decode_one(instr_t* raw, Instruction& out) {
    if (instr_num_dsts(raw) != 1) return false;
    const auto dst = instr_get_dst(raw, 0);
    if (!opnd_is_reg(dst) || opnd_get_size(dst) != OPSZ_8) return false;
    // Address-size and segment prefixes change LEA/MOV interpretation.
    // Check the raw prefix run; reject rather than approximate the effect.
    const auto* pc = instr_get_app_pc(raw);
    if (!pc) return false;
    for (unsigned i=0;i<15;++i) {
        byte v{}; if (!dr_safe_read(pc+i,1,&v,nullptr)) return false;
        // Only a single REX prefix is part of the admitted encoding.
        if (v>=0x40 && v<=0x4f) {
            if (i != 0) return false;
            continue;
        }
        if (v==0x66 || v==0x67 || v==0xf2 || v==0xf3 || v==0xf0 ||
            v==0x2e || v==0x36 || v==0x3e || v==0x26 || v==0x64 || v==0x65) return false;
        break;
    }
    const int n = reg_index(opnd_get_reg(dst));
    if (n < 0 || n == 4) return false;
    out.dst = static_cast<Reg>(n);
    const int op = instr_get_opcode(raw);
    if (op == OP_lea) {
        if (instr_num_srcs(raw) != 1) return false;
        const auto mem = instr_get_src(raw, 0);
        if (!opnd_is_base_disp(mem)) return false;
        const reg_id_t base = opnd_get_base(mem), index = opnd_get_index(mem);
        if (base == DR_REG_NULL) return false; // reject ambiguous RIP/absolute forms
        else { const int b = reg_index(base); if (b < 0) return false; out.a = Operand::reg(static_cast<Reg>(b)); }
        if (index == DR_REG_NULL) out.b = Operand::imm(0);
        else { const int i = reg_index(index); if (i < 0) return false; out.b = Operand::reg(static_cast<Reg>(i)); }
        out.op = Op::Lea;
        out.scale = index == DR_REG_NULL ? 1 : static_cast<std::uint8_t>(opnd_get_scale(mem));
        out.displacement = opnd_get_disp(mem);
        return valid_scale(out.scale);
    }
    // MOV is the first admitted x64 subset. Arithmetic changes flags and
    // requires a separate complete liveness proof across block successors.
    if (op != OP_mov_ld && op != OP_mov_st && op != OP_mov_imm) return false;
    if (instr_num_srcs(raw) != 1 || !source(instr_get_src(raw, 0), out.a)) return false;
    out.op = Op::Mov;
    return true;
}
inline bool decode_block(instrlist_t* bb, Decoded& out) {
    instr_t* first = nullptr;
    for (auto* ins = instrlist_first(bb); ins; ins = instr_get_next(ins)) {
        if (!instr_is_app(ins)) continue;
        ++out.app_count;
        if (!first) first = ins;
        if (out.region.count >= max_instructions || !instr_get_app_pc(ins)) break;
        if (out.region.count && instr_get_app_pc(ins)!=out.end) break;
        Instruction decoded{};
        if (!decode_one(ins, decoded)) break;
        out.region.instructions[out.region.count++] = decoded;
        out.region.live_out_mask |= reg_bit(decoded.dst); // conservative: all writes
        out.end = instr_get_app_pc(ins) + instr_length(dr_get_current_drcontext(), ins);
    }
    if (!first || out.region.count < 2) return false;
    out.start = instr_get_app_pc(first);
    out.byte_count=static_cast<std::size_t>(out.end-out.start);
    if (!out.byte_count || out.byte_count>out.original_bytes.size()) return false;
    out.region.flags_dead_on_exit = false;
    out.region.proven_no_external_effects = true;
    // Identity includes every original byte. Read failure declines the block.
    std::uint64_t hash = 14695981039346656037ull;
    for (auto* p = out.start; p < out.end; ++p) {
        byte v{};
        if (!dr_safe_read(p, 1, &v, nullptr)) return false;
        out.original_bytes[static_cast<std::size_t>(p-out.start)]=v;
        hash = (hash ^ v) * 1099511628211ull;
    }
    out.code_hash = hash;
    return analyze(out.region).accepted();
}
}
