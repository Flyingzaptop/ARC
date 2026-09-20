#pragma once

#include <d3d12.h>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace arc::dx12::binding {
// Decode only complete compute streams. Unknown/graphics subobjects and
// duplicate fields are never treated as an equivalent compute pipeline.
std::optional<D3D12_COMPUTE_PIPELINE_STATE_DESC> compute_stream(const D3D12_PIPELINE_STATE_STREAM_DESC&);

struct Range {
    D3D12_DESCRIPTOR_RANGE_TYPE type{};
    UINT first_register{}, space{}, count{}, table_offset{};
    D3D12_DESCRIPTOR_RANGE_FLAGS flags{};
};
struct Parameter {
    D3D12_ROOT_PARAMETER_TYPE type{};
    D3D12_SHADER_VISIBILITY visibility{};
    UINT shader_register{}, space{}, constants{};
    std::vector<Range> ranges;
};
struct Location {
    UINT parameter{}, table_offset{};
    bool table{}, constants{}, static_sampler{};
    D3D12_STATIC_SAMPLER_DESC sampler{};
};

// Owns every deserialized field: no pointer into application memory survives.
struct Layout {
    D3D12_ROOT_SIGNATURE_FLAGS flags{};
    std::vector<Parameter> parameters;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    UINT dwords{};
    bool complete{};
    std::string rejection;

    static Layout parse(std::span<const std::byte> bytes);
    [[nodiscard]] std::optional<Location> locate(D3D12_DESCRIPTOR_RANGE_TYPE type,
        UINT shader_register, UINT space, D3D12_SHADER_VISIBILITY stage) const noexcept;
};

// Append b0 in an unused space without changing existing parameter indices,
// range flags, static samplers, or root flags. Empty result means unsupported.
std::vector<std::byte> append_control_cbv(std::span<const std::byte> original, UINT space);

struct Argument {
    D3D12_ROOT_PARAMETER_TYPE type{};
    UINT64 address{};
    std::array<UINT, 64> words{};
    std::uint64_t written{};
    bool initialized{};
};

// One graphics OR compute root namespace for one native recording generation.
// Missing state is never filled with zero and advertised as a valid binding.
class Arguments {
public:
    void reset() noexcept;
    void signature(std::uint64_t identity, std::shared_ptr<const Layout> layout);
    bool table(UINT parameter, D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept;
    bool descriptor(UINT parameter, D3D12_ROOT_PARAMETER_TYPE type, UINT64 address) noexcept;
    bool constants(UINT parameter, UINT offset, std::span<const UINT> words) noexcept;
    void invalidate_tables() noexcept;
    // Borrowed until the next mutation; callers hold the recording lock or own
    // an immutable submission snapshot. Avoid copying 64 constants per lookup.
    [[nodiscard]] const Argument* argument(UINT parameter) const noexcept;
    [[nodiscard]] const Argument* raw_argument(UINT parameter) const noexcept;
    [[nodiscard]] std::optional<Location> locate(D3D12_DESCRIPTOR_RANGE_TYPE type,
        UINT shader_register, UINT space, D3D12_SHADER_VISIBILITY stage) const noexcept;
    [[nodiscard]] std::uint64_t identity() const noexcept { return identity_; }
    [[nodiscard]] const std::shared_ptr<const Layout>& layout() const noexcept { return layout_; }
private:
    std::uint64_t identity_{};
    std::shared_ptr<const Layout> layout_;
    // Most signatures have only a few parameters. A fixed 64 * 64-word array
    // made every command-list lifetime/reset zero and copy ~18 KiB needlessly.
    std::vector<Argument> arguments_;
};

} // namespace arc::dx12::binding
