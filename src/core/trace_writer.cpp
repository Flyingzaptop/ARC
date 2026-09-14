#include "arc/trace.hpp"

#include <fstream>
#include <memory>
#include <cstring>

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

class TraceWriter::Impl final {
public:
    explicit Impl(const std::filesystem::path& path) : output(path, std::ios::binary | std::ios::trunc) {}
    std::ofstream output;
    std::uint32_t next_chunk{};
};

TraceWriter::TraceWriter(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(path)) {}
TraceWriter::~TraceWriter() = default;

bool TraceWriter::append(const std::span<const Event> events) {
    if (!good() || events.empty()) {
        return events.empty() && good();
    }
    std::size_t total{};
    for (const auto& event : events) {
        if (event.header.payload_bytes > kMaxEventPayloadBytes) { return false; }
        total += sizeof(EventHeader) + event.header.payload_bytes;
        if (total > kMaxTraceChunkBytes) { return false; }
    }
    std::vector<std::byte> packed(total);
    std::size_t offset{};
    for (const auto& event : events) {
        std::memcpy(packed.data() + offset, &event.header, sizeof(EventHeader)); offset += sizeof(EventHeader);
        std::memcpy(packed.data() + offset, event.payload.data(), event.header.payload_bytes); offset += event.header.payload_bytes;
    }
    const std::span<const std::byte> payload(packed);
    const TraceChunkHeader header{
        .chunk_sequence = impl_->next_chunk++,
        .payload_bytes = static_cast<std::uint32_t>(payload.size_bytes()),
        .checksum = fnv1a(payload),
    };
    impl_->output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    impl_->output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size_bytes()));
    impl_->output.flush();
    return good();
}

bool TraceWriter::good() const noexcept { return impl_ && impl_->output.good(); }

}  // namespace arc
