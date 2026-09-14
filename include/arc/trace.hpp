#pragma once

#include "arc/events.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace arc {

inline constexpr std::uint32_t kTraceSchemaVersion = 3;
inline constexpr std::uint32_t kMaxTraceChunkBytes = 16 * 1024 * 1024;
inline constexpr std::uint64_t kTraceChunkMagic = 0x314B4E4843524141ULL; // "ARCHNK1"

struct TraceChunkHeader final {
    std::uint64_t magic{kTraceChunkMagic};
    std::uint32_t schema{kTraceSchemaVersion};
    std::uint32_t chunk_sequence{};
    std::uint32_t payload_bytes{};
    std::uint32_t checksum{};
};
static_assert(sizeof(TraceChunkHeader) == 24);

class TraceWriter final {
public:
    explicit TraceWriter(const std::filesystem::path& path);
    ~TraceWriter();
    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;

    [[nodiscard]] bool append(std::span<const Event> events);
    [[nodiscard]] bool good() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class TraceReader final {
public:
    enum class Status { Complete, TruncatedTail, CorruptTail, SchemaMismatch, IoError };
    struct Result {
        std::vector<Event> events;
        Status status{Status::Complete};
        std::uint32_t complete_chunks{};
    };
    [[nodiscard]] static Result inspect(const std::filesystem::path& path);
    // Reads complete valid chunks only. A partially written final chunk is
    // intentionally ignored so crash recovery preserves earlier data.
    [[nodiscard]] static std::vector<Event> read_recoverable(const std::filesystem::path& path);
};

}  // namespace arc
