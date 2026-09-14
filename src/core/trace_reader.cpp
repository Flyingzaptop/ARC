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

TraceReader::Result TraceReader::inspect(const std::filesystem::path& path) {
    Result result;
    std::ifstream input(path, std::ios::binary);
    if (!input) { result.status = Status::IoError; return result; }
    while (input.good()) {
        TraceChunkHeader header{};
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (input.gcount() == 0 && input.eof()) { break; }
        if (input.gcount() != sizeof(header)) { result.status = Status::TruncatedTail; break; }
        if (header.schema != kTraceSchemaVersion) { result.status = Status::SchemaMismatch; break; }
        if (header.magic != kTraceChunkMagic || header.chunk_sequence != result.complete_chunks ||
            header.payload_bytes == 0 || header.payload_bytes > kMaxTraceChunkBytes ||
            header.payload_bytes % sizeof(Event) != 0) { result.status = Status::CorruptTail; break; }
        std::vector<std::byte> payload(header.payload_bytes);
        input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (input.gcount() != static_cast<std::streamsize>(payload.size())) { result.status = Status::TruncatedTail; break; }
        if (fnv1a(payload) != header.checksum) { result.status = Status::CorruptTail; break; }
        const auto old_size = result.events.size();
        result.events.resize(old_size + payload.size() / sizeof(Event));
        std::memcpy(result.events.data() + old_size, payload.data(), payload.size());
        bool valid = true;
        for (std::size_t i = old_size; i < result.events.size(); ++i) {
            if (result.events[i].header.payload_bytes > kMaxEventPayloadBytes) { valid = false; break; }
        }
        if (!valid) { result.events.resize(old_size); result.status = Status::CorruptTail; break; }
        ++result.complete_chunks;
    }
    return result;
}

std::vector<Event> TraceReader::read_recoverable(const std::filesystem::path& path) {
    return inspect(path).events;
}

}  // namespace arc
