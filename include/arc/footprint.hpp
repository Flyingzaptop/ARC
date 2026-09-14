#pragma once
#include <cstdint>
#include <optional>
#include <vector>
namespace arc {
struct TexelLayout { std::uint32_t block_width{1}, block_height{1}, bytes_per_block{4}; };
struct SubresourceFootprint {
    std::uint32_t mip{}, layer{}, width{}, height{}, depth{};
    std::uint64_t logical_bytes{}, row_bytes{}, aligned_row_bytes{};
};
// Logical texel payload and D3D12-style copy row alignment are estimates,
// never physical allocation sizes. Multi-plane formats require per-plane layouts.
inline std::optional<std::vector<SubresourceFootprint>> estimate_footprints(
    std::uint32_t width, std::uint32_t height, std::uint32_t depth, std::uint16_t mips,
    std::uint16_t layers, std::uint32_t samples, TexelLayout layout) {
    if (!width || !height || !depth || !mips || !layers || !samples || !layout.block_width || !layout.block_height || !layout.bytes_per_block || mips > 32) { return std::nullopt; }
    std::vector<SubresourceFootprint> result;
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        auto w = width, h = height, d = depth;
        for (std::uint32_t mip = 0; mip < mips; ++mip) {
            const auto row = ((static_cast<std::uint64_t>(w) + layout.block_width - 1) / layout.block_width) * layout.bytes_per_block;
            const auto rows = (static_cast<std::uint64_t>(h) + layout.block_height - 1) / layout.block_height;
            if (row > UINT64_MAX / rows / d / samples) { return std::nullopt; }
            result.push_back({mip, layer, w, h, d, row * rows * d * samples, row, (row + 255) & ~255ULL});
            w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; d = d > 1 ? d / 2 : 1;
        }
    }
    return result;
}
}
