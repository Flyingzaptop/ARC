#pragma once

bool exemptByte(uint64_t a, const Pack& p, const EntryState& entry) {
    if (entry.rsp >= 8 && entry.rsp - 8 <= a && a < entry.rsp) return true;
    const uint64_t base = p.moduleBase;
    for (uint32_t rva : p.gsSites)
        if (base + rva <= a && a < base + rva + 9) return true;
    for (uint32_t rva : p.trapSites)
        if (a == base + rva) return true;
    return base + p.loopEnd <= a && a < base + p.loopEnd + 14;
}

struct EffectAudit {
    size_t changedAllowed = 0;
    size_t changedOutside = 0;
    uint64_t firstOutside = 0;
};

const Region& stackRegion(const Pack& p, const EntryState& entry) {
    auto found = std::find_if(p.regions.begin(), p.regions.end(),
        [&entry](const Region& r) {
            return r.base <= entry.rsp && entry.rsp < r.base + r.size;
        });
    if (found == p.regions.end())
        throw std::runtime_error("entry_stack_region_missing");
    return *found;
}

EffectAudit auditCloneEffects(const Pack& p, const EntryState& entry,
                              uint64_t initialCursor) {
    const Region& stack = stackRegion(p, entry);
    const uint64_t cursorHeader = p.owner + p.cursorOffset;
    const uint64_t outputStart = p.version == 3 ? p.outputStart : initialCursor;
    const uint64_t outputEnd = p.version == 3 ? p.outputEnd : p.expectedFinalCursor;
    EffectAudit result;
    for (const auto& region : p.regions) {
        auto before = fileBytes(region.before, kMaxRegion);
        if (before.size() != region.size)
            throw std::runtime_error("before_region_size_changed");
        const auto* actual = reinterpret_cast<const uint8_t*>(region.base);
        if (std::memcmp(actual, before.data(), before.size()) == 0)
            continue;
        for (size_t i = 0; i < before.size(); ++i) {
            if (actual[i] == before[i]) continue;
            const uint64_t a = region.base + i;
            const bool allowed =
                (stack.base <= a && a < stack.base + stack.size) ||
                (p.version == 2 && cursorHeader <= a && a < cursorHeader + 8) ||
                (outputStart <= a && a < outputEnd) ||
                exemptByte(a, p, entry);
            if (allowed) {
                ++result.changedAllowed;
            } else {
                if (!result.changedOutside) result.firstOutside = a;
                ++result.changedOutside;
            }
        }
    }
    return result;
}

void resetWarmState(const Pack& p, const EntryState& entry,
                    uint64_t initialCursor) {
    const Region& stack = stackRegion(p, entry);
    auto stackBefore = fileBytes(stack.before, kMaxRegion);
    if (stackBefore.size() != stack.size)
        throw std::runtime_error("before_stack_size_changed");
    std::memcpy(reinterpret_cast<void*>(stack.base),
                stackBefore.data(), stackBefore.size());
    if (p.version == 2)
        std::memcpy(reinterpret_cast<void*>(p.owner + p.cursorOffset),
                    &initialCursor, 8);
}

size_t compareAfter(const Pack& p, uint64_t initialCursor) {
    size_t mismatches = 0;
    const uint64_t header = p.owner + p.cursorOffset;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    if (p.version == 2) {
        ranges.emplace_back(header, header + 8);
        ranges.emplace_back(initialCursor, p.expectedFinalCursor);
    } else {
        ranges.emplace_back(p.outputStart, p.outputEnd);
        ranges.insert(ranges.end(), p.stackWriteRanges.begin(),
                      p.stackWriteRanges.end());
    }
    for (const auto& [lo, hi] : ranges) {
        if (hi < lo) throw std::runtime_error("after_range_invalid");
        if (lo == hi) continue;
        auto region = std::find_if(p.regions.begin(), p.regions.end(),
            [lo, hi](const Region& r) {
                return r.base <= lo && hi <= r.base + r.size;
            });
        if (region == p.regions.end())
            throw std::runtime_error("after_range_outside_snapshot");
        auto expected = fileBytes(region->after, kMaxRegion);
        if (expected.size() != region->size)
            throw std::runtime_error("after_region_size_changed");
        const auto* actual = reinterpret_cast<const uint8_t*>(lo);
        const size_t offset = static_cast<size_t>(lo - region->base);
        for (uint64_t j = 0; j < hi - lo; ++j)
            if (actual[j] != expected[offset + j]) ++mismatches;
    }
    return mismatches;
}

void compareExitContext(const Pack& p) {
    CONTEXT expected{};
    std::memcpy(&expected, p.exitContext.data(), sizeof(expected));
    if (expected.Rip != p.moduleBase + p.loopEnd ||
        g_exitState.rax != expected.Rax ||
        g_exitState.rcx != expected.Rcx ||
        g_exitState.rdx != expected.Rdx ||
        g_exitState.rbx != expected.Rbx ||
        g_exitState.rsp != expected.Rsp ||
        g_exitState.rbp != expected.Rbp ||
        g_exitState.rsi != expected.Rsi ||
        g_exitState.rdi != expected.Rdi ||
        g_exitState.r8 != expected.R8 ||
        g_exitState.r9 != expected.R9 ||
        g_exitState.r10 != expected.R10 ||
        g_exitState.r11 != expected.R11 ||
        g_exitState.r12 != expected.R12 ||
        g_exitState.r13 != expected.R13 ||
        g_exitState.r14 != expected.R14 ||
        g_exitState.r15 != expected.R15 ||
        g_exitState.mxcsr != expected.MxCsr ||
        std::memcmp(g_exitState.xmm, &expected.Xmm0, sizeof(g_exitState.xmm)))
        throw std::runtime_error("natural_exit_context_mismatch");
}
