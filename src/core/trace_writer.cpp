#include "arc/trace.hpp"
#include "arc/clock.hpp"

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
    explicit Impl(const std::filesystem::path& path, TraceWriterOptions configured)
        : output(path, std::ios::binary | std::ios::trunc), options(configured) {
        packed.reserve(1024 * 1024);
    }
    std::ofstream output;
    TraceWriterOptions options;
    TraceWriterStatistics statistics;
    std::vector<std::byte> packed;
    std::uint32_t next_chunk{};
    std::uint32_t chunks_since_checkpoint{};
};

TraceWriter::TraceWriter(const std::filesystem::path& path, TraceWriterOptions options) : impl_(std::make_unique<Impl>(path, options)) {}
TraceWriter::~TraceWriter() = default;

bool TraceWriter::append(const std::span<const Event> events) {
    const auto cpuStart = current_thread_cpu_time_ns();
    if (!good() || events.empty()) {
        return events.empty() && good();
    }
    std::size_t total{};
    for (const auto& event : events) {
        if (event.header.payload_bytes > kMaxEventPayloadBytes) { return false; }
        total += sizeof(EventHeader) + event.header.payload_bytes;
        if (total > kMaxTraceChunkBytes) { return false; }
    }
    impl_->packed.resize(total);
    std::size_t offset{};
    for (const auto& event : events) {
        std::memcpy(impl_->packed.data() + offset, &event.header, sizeof(EventHeader)); offset += sizeof(EventHeader);
        std::memcpy(impl_->packed.data() + offset, event.payload.data(), event.header.payload_bytes); offset += event.header.payload_bytes;
    }
    const std::span<const std::byte> payload(impl_->packed);
    const TraceChunkHeader header{
        .chunk_sequence = impl_->next_chunk++,
        .payload_bytes = static_cast<std::uint32_t>(payload.size_bytes()),
        .checksum = fnv1a(payload),
    };
    impl_->output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    impl_->output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size_bytes()));
    ++impl_->statistics.chunks;
    impl_->statistics.payload_bytes += payload.size_bytes();
    impl_->statistics.packing_capacity = (std::max)(impl_->statistics.packing_capacity, impl_->packed.capacity());
    ++impl_->chunks_since_checkpoint;
    if (impl_->options.checkpoint_every_chunks && impl_->chunks_since_checkpoint >= impl_->options.checkpoint_every_chunks) {
        if (!checkpoint()) { return false; }
    }
    const auto cpuEnd = current_thread_cpu_time_ns();
    if (cpuEnd >= cpuStart) { impl_->statistics.cpu_ns += cpuEnd - cpuStart; }
    return good();
}

bool TraceWriter::checkpoint() {
    impl_->output.flush();
    impl_->chunks_since_checkpoint = 0;
    ++impl_->statistics.checkpoints;
    return good();
}

bool TraceWriter::good() const noexcept { return impl_ && impl_->output.good(); }
TraceWriterStatistics TraceWriter::statistics() const noexcept { return impl_ ? impl_->statistics : TraceWriterStatistics{}; }

}  // namespace arc
