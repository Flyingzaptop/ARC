#include "arc/trace.hpp"

#include <cstring>
#include <fstream>

namespace arc {
namespace {

std::uint32_t fnv1a(std::span<const std::byte> bytes) noexcept {
    std::uint32_t value = 2166136261U;
    for (const auto byte : bytes) {
        value ^= static_cast<std::uint8_t>(byte);
        value *= 16777619U;
    }
    return value;
}

}  // namespace

std::vector<Event> TraceReader::read_recoverable(const std::filesystem::path& path) {
    std::vector<Event> result;
    std::ifstream input(path, std::ios::binary);
    while (input.good()) {
        TraceChunkHeader header{};
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (input.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
            header.magic != kTraceChunkMagic || header.schema != kTraceSchemaVersion ||
            header.payload_bytes % sizeof(Event) != 0) {
            break;
        }
        std::vector<std::byte> payload(header.payload_bytes);
        input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (input.gcount() != static_cast<std::streamsize>(payload.size()) || fnv1a(payload) != header.checksum) {
            break;
        }
        const auto old_size = result.size();
        result.resize(old_size + payload.size() / sizeof(Event));
        std::memcpy(result.data() + old_size, payload.data(), payload.size());
    }
    return result;
}

}  // namespace arc
