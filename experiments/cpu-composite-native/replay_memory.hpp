#pragma once

struct Region {
    uint64_t base, size;
    std::filesystem::path before, after;
    DWORD protect = PAGE_EXECUTE_READWRITE;
};
struct ExpectedCode { uint32_t rva; std::vector<uint8_t> bytes; };
struct Pack {
    uint32_t version = 0;
    uint64_t moduleBase, tlsPointer, owner, expectedFinalCursor;
    uint32_t loopEntry, loopEnd, appendEntry, appendEnd;
    uint32_t cursorOffset, capacityOffset, stride, packetCount;
    uint32_t fullRecordZero;
    std::filesystem::path modulePath;
    std::vector<uint8_t> context;
    std::vector<uint8_t> exitContext;
    std::vector<uint32_t> gsSites, trapSites;
    std::vector<Region> regions;
    std::vector<ExpectedCode> code;
    uint64_t outputPointerAddress = 0, outputBase = 0;
    uint64_t outputStart = 0, outputEnd = 0;
    std::vector<std::pair<uint64_t, uint64_t>> stackWriteRanges;
};

class Reader {
public:
    explicit Reader(const std::filesystem::path& path) : file_(path, std::ios::binary) {
        if (!file_) throw std::runtime_error("pack_open_failed");
    }
    template<class T> T scalar() {
        T value{};
        file_.read(reinterpret_cast<char*>(&value), sizeof(value));
        if (!file_) throw std::runtime_error("truncated_pack");
        return value;
    }
    std::vector<uint8_t> bytes(size_t n) {
        if (n > kMaxRegion) throw std::runtime_error("pack_item_too_large");
        std::vector<uint8_t> data(n);
        file_.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(n));
        if (!file_) throw std::runtime_error("truncated_pack");
        return data;
    }
    std::filesystem::path path() {
        uint32_t n = scalar<uint32_t>();
        if (!n || n > 32768) throw std::runtime_error("bad_path_length");
        auto b = bytes(n);
        return std::filesystem::u8path(std::string(b.begin(), b.end()));
    }
    bool atEnd() { return file_.peek() == EOF; }
private:
    std::ifstream file_;
};

Pack readPack(const std::filesystem::path& path) {
    Reader r(path);
    const auto magic = r.bytes(8);
    if (std::string(magic.begin(), magic.end()) != "ARCRPL01")
        throw std::runtime_error("bad_pack_magic");
    Pack p{};
    p.version = r.scalar<uint32_t>();
    if (p.version != 2 && p.version != 3)
        throw std::runtime_error("bad_pack_version");
    p.moduleBase = r.scalar<uint64_t>();
    p.loopEntry = r.scalar<uint32_t>(); p.loopEnd = r.scalar<uint32_t>();
    p.appendEntry = r.scalar<uint32_t>(); p.appendEnd = r.scalar<uint32_t>();
    p.tlsPointer = r.scalar<uint64_t>(); p.owner = r.scalar<uint64_t>();
    p.cursorOffset = r.scalar<uint32_t>(); p.capacityOffset = r.scalar<uint32_t>();
    p.stride = r.scalar<uint32_t>(); p.packetCount = r.scalar<uint32_t>();
    p.expectedFinalCursor = r.scalar<uint64_t>();
    p.fullRecordZero = r.scalar<uint32_t>();
    p.modulePath = r.path();
    const uint32_t contextSize = r.scalar<uint32_t>();
    if (contextSize != sizeof(CONTEXT)) throw std::runtime_error("context_size_mismatch");
    p.context = r.bytes(contextSize);
    const uint32_t exitContextSize = r.scalar<uint32_t>();
    if (exitContextSize != sizeof(CONTEXT))
        throw std::runtime_error("exit_context_size_mismatch");
    p.exitContext = r.bytes(exitContextSize);
    auto array = [&r]() {
        uint32_t count = r.scalar<uint32_t>();
        if (count > kMaxInstructions) throw std::runtime_error("array_too_large");
        std::vector<uint32_t> values(count);
        for (auto& value : values) value = r.scalar<uint32_t>();
        return values;
    };
    p.gsSites = array(); p.trapSites = array();
    uint32_t regions = r.scalar<uint32_t>();
    if (!regions || regions > kMaxRegions) throw std::runtime_error("region_count_invalid");
    for (uint32_t i = 0; i < regions; ++i) {
        Region q{};
        q.base = r.scalar<uint64_t>(); q.size = r.scalar<uint64_t>();
        q.before = r.path(); q.after = r.path();
        if (p.version == 3) q.protect = r.scalar<uint32_t>();
        if (!q.base || !q.size || q.size > kMaxRegion ||
            q.base + q.size < q.base) throw std::runtime_error("region_span_invalid");
        p.regions.push_back(q);
    }
    uint32_t codeCount = r.scalar<uint32_t>();
    if (codeCount > kMaxInstructions) throw std::runtime_error("code_count_invalid");
    for (uint32_t i = 0; i < codeCount; ++i) {
        ExpectedCode q{};
        q.rva = r.scalar<uint32_t>();
        uint32_t n = r.scalar<uint32_t>();
        if (!n || n > 16) throw std::runtime_error("instruction_size_invalid");
        q.bytes = r.bytes(n);
        p.code.push_back(std::move(q));
    }
    if (p.version == 3) {
        p.outputPointerAddress = r.scalar<uint64_t>();
        p.outputBase = r.scalar<uint64_t>();
        p.outputStart = r.scalar<uint64_t>();
        p.outputEnd = r.scalar<uint64_t>();
        uint32_t n = r.scalar<uint32_t>();
        if (n > 256) throw std::runtime_error("stack_range_count_invalid");
        for (uint32_t i = 0; i < n; ++i) {
            uint64_t lo = r.scalar<uint64_t>(), hi = r.scalar<uint64_t>();
            if (lo >= hi) throw std::runtime_error("stack_range_invalid");
            p.stackWriteRanges.emplace_back(lo, hi);
        }
    }
    if (!r.atEnd()) throw std::runtime_error("trailing_pack_bytes");
    if (p.loopEntry >= p.loopEnd || !p.stride || !p.packetCount ||
        (p.version == 2 && (p.appendEntry >= p.appendEnd ||
                            p.gsSites.size() != 2)) ||
        (p.version == 3 && (!p.gsSites.empty() || p.stride != 4 ||
                            p.outputStart >= p.outputEnd)))
        throw std::runtime_error("pack_control_invalid");
    return p;
}

std::vector<uint8_t> fileBytes(const std::filesystem::path& path, uint64_t maxSize) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("snapshot_open_failed");
    auto end = f.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > maxSize)
        throw std::runtime_error("snapshot_size_limit");
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(end));
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(end));
    if (!f) throw std::runtime_error("snapshot_read_failed");
    return data;
}

void* exactAlloc(uint64_t base, uint64_t bytes,
                 DWORD protection = PAGE_EXECUTE_READWRITE) {
    void* p = VirtualAlloc(reinterpret_cast<void*>(base), static_cast<SIZE_T>(bytes),
                           MEM_RESERVE | MEM_COMMIT, protection);
    if (reinterpret_cast<uint64_t>(p) != base) {
        const DWORD error = GetLastError();
        std::ostringstream detail;
        detail << "exact_address_VirtualAlloc_collision requested=0x"
               << std::hex << base << " bytes=0x" << bytes
               << " protection=0x" << protection
               << " returned=0x" << reinterpret_cast<uint64_t>(p)
               << " helper_exe_base=0x"
               << reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr))
               << std::dec << " GetLastError=" << error;
        throw std::runtime_error(detail.str());
    }
    return p;
}

struct Module {
    uint64_t base;
    uint32_t size;
};
Module mapPe(const Pack& p) {
    auto file = fileBytes(p.modulePath, kMaxRegion);
    if (file.size() < sizeof(IMAGE_DOS_HEADER))
        throw std::runtime_error("PE_too_short");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size())
        throw std::runtime_error("PE_header_invalid");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        throw std::runtime_error("not_PE64_AMD64");
    uint32_t size = nt->OptionalHeader.SizeOfImage;
    if (!size || size > kMaxRegion ||
        nt->OptionalHeader.SizeOfHeaders > file.size())
        throw std::runtime_error("PE_image_size_invalid");
    auto* image = static_cast<uint8_t*>(exactAlloc(p.moduleBase, size));
    std::memcpy(image, file.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    if (reinterpret_cast<const uint8_t*>(sections + nt->FileHeader.NumberOfSections) >
        file.data() + file.size()) throw std::runtime_error("PE_sections_truncated");
    for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = sections[i];
        if (s.VirtualAddress > size || s.Misc.VirtualSize > size - s.VirtualAddress ||
            s.PointerToRawData > file.size() ||
            s.SizeOfRawData > file.size() - s.PointerToRawData ||
            s.SizeOfRawData > size - s.VirtualAddress)
            throw std::runtime_error("PE_section_out_of_bounds");
        std::memcpy(image + s.VirtualAddress, file.data() + s.PointerToRawData,
                    s.SizeOfRawData);
    }
    const int64_t delta = static_cast<int64_t>(p.moduleBase - nt->OptionalHeader.ImageBase);
    if (delta) {
        const auto& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (!dir.VirtualAddress || dir.VirtualAddress + dir.Size > size)
            throw std::runtime_error("PE_relocations_missing");
        uint32_t at = dir.VirtualAddress;
        while (at < dir.VirtualAddress + dir.Size) {
            if (at + sizeof(IMAGE_BASE_RELOCATION) > size)
                throw std::runtime_error("PE_relocation_block_invalid");
            const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(image + at);
            if (block->SizeOfBlock < sizeof(*block) ||
                at + block->SizeOfBlock > dir.VirtualAddress + dir.Size)
                throw std::runtime_error("PE_relocation_block_invalid");
            auto* entries = reinterpret_cast<const WORD*>(block + 1);
            const size_t count = (block->SizeOfBlock - sizeof(*block)) / sizeof(WORD);
            for (size_t j = 0; j < count; ++j) {
                uint16_t type = entries[j] >> 12;
                uint32_t rva = block->VirtualAddress + (entries[j] & 0xfff);
                if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
                if (type != IMAGE_REL_BASED_DIR64 || rva + 8 > size)
                    throw std::runtime_error("PE_relocation_type_unsupported");
                auto* word = reinterpret_cast<uint64_t*>(image + rva);
                *word += delta;
            }
            at += block->SizeOfBlock;
        }
    }
    return {p.moduleBase, size};
}

void restoreRegions(const Pack& p, const Module& module) {
    const uint64_t moduleEnd = module.base + module.size;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const uint64_t granularity = system.dwAllocationGranularity;
    if (!granularity || (granularity & (granularity - 1)))
        throw std::runtime_error("allocation_granularity_invalid");
    struct Reservation { uint64_t lo, hi; DWORD protect; };
    std::vector<Reservation> reservations;
    for (const auto& r : p.regions) {
        const bool overlaps = r.base < moduleEnd && module.base < r.base + r.size;
        if (overlaps && !(module.base <= r.base && r.base + r.size <= moduleEnd))
            throw std::runtime_error("region_partially_overlaps_module");
        if (!overlaps) {
            const uint64_t lo = r.base & ~(granularity - 1);
            const uint64_t hi = (r.base + r.size + granularity - 1) &
                                ~(granularity - 1);
            if (lo < moduleEnd && module.base < hi)
                throw std::runtime_error("region_reservation_overlaps_module");
            DWORD protection = r.protect;
            if (protection != PAGE_EXECUTE_READWRITE &&
                protection != PAGE_READWRITE &&
                protection != (PAGE_READWRITE | PAGE_WRITECOMBINE))
                throw std::runtime_error("region_protection_unsupported");
            reservations.push_back({lo, hi, protection});
        }
    }
    std::sort(reservations.begin(), reservations.end(),
              [](const Reservation& a, const Reservation& b) {
                  return a.lo < b.lo;
              });
    std::vector<Reservation> merged;
    for (const auto& span : reservations) {
        if (!merged.empty() && span.lo <= merged.back().hi) {
            if (span.protect != merged.back().protect)
                throw std::runtime_error("conflicting_region_protection");
            merged.back().hi = std::max(merged.back().hi, span.hi);
        } else merged.push_back(span);
    }
    for (const auto& span : merged)
        exactAlloc(span.lo, span.hi - span.lo, span.protect);
    for (const auto& r : p.regions) {
        auto data = fileBytes(r.before, kMaxRegion);
        if (data.size() != r.size)
            throw std::runtime_error("before_region_size_changed");
        std::memcpy(reinterpret_cast<void*>(r.base), data.data(), data.size());
    }
}

void resetRegions(const Pack& p) {
    for (const auto& r : p.regions) {
        auto data = fileBytes(r.before, kMaxRegion);
        if (data.size() != r.size)
            throw std::runtime_error("before_region_size_changed");
        std::memcpy(reinterpret_cast<void*>(r.base), data.data(), data.size());
    }
}
