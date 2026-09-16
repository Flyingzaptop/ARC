#include "arc/dx12_observer.hpp"
#include "arc/texture_quality.hpp"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT64 kTileBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
constexpr UINT kTextureWidth = 4096;
constexpr UINT kTextureHeight = 4096;
constexpr DXGI_FORMAT kTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(what) + " failed with HRESULT " + std::to_string(static_cast<unsigned>(hr)));
    }
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

UINT mip_count(UINT width, UINT height) {
    UINT result = 1;
    while (width > 1 || height > 1) {
        width = (std::max)(1U, width >> 1U);
        height = (std::max)(1U, height >> 1U);
        ++result;
    }
    return result;
}

struct TileRegion {
    UINT first_subresource{};
    D3D12_TILED_RESOURCE_COORDINATE coordinate{};
    D3D12_TILE_REGION_SIZE size{};
    UINT tile_count{};
    bool packed{};
    ComPtr<ID3D12Heap> heap;
};

struct TransferBuffers {
    UINT mip{};
    std::uint8_t pattern{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_bytes{};
    UINT64 total_bytes{};
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> readback;
};

ComPtr<ID3D12Heap> create_tile_heap(ID3D12Device* device, UINT tile_count) {
    D3D12_HEAP_DESC description{};
    description.SizeInBytes = static_cast<UINT64>(tile_count) * kTileBytes;
    description.Properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    description.Alignment = 0;
    description.Flags = static_cast<D3D12_HEAP_FLAGS>(
        D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES);
    ComPtr<ID3D12Heap> heap;
    check(device->CreateHeap(&description, IID_PPV_ARGS(&heap)), "CreateHeap");
    return heap;
}

TransferBuffers create_transfer(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& texture_description,
    UINT mip,
    std::uint8_t pattern) {
    TransferBuffers transfer{};
    transfer.mip = mip;
    transfer.pattern = pattern;
    device->GetCopyableFootprints(
        &texture_description,
        mip,
        1,
        0,
        &transfer.footprint,
        &transfer.rows,
        &transfer.row_bytes,
        &transfer.total_bytes);
    if (transfer.total_bytes == 0 || transfer.row_bytes == 0 || transfer.rows == 0) {
        throw std::runtime_error("invalid copyable footprint");
    }

    const auto description = buffer_desc(transfer.total_bytes);
    auto upload_properties = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    auto readback_properties = heap_properties(D3D12_HEAP_TYPE_READBACK);
    check(device->CreateCommittedResource(
        &upload_properties,
        D3D12_HEAP_FLAG_NONE,
        &description,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&transfer.upload)), "Create upload buffer");
    check(device->CreateCommittedResource(
        &readback_properties,
        D3D12_HEAP_FLAG_NONE,
        &description,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&transfer.readback)), "Create readback buffer");

    void* mapped = nullptr;
    D3D12_RANGE empty_range{0, 0};
    check(transfer.upload->Map(0, &empty_range, &mapped), "Map upload buffer");
    auto* bytes = static_cast<std::uint8_t*>(mapped) + transfer.footprint.Offset;
    for (UINT row = 0; row < transfer.rows; ++row) {
        std::memset(
            bytes + static_cast<std::size_t>(row) * transfer.footprint.Footprint.RowPitch,
            pattern,
            static_cast<std::size_t>(transfer.row_bytes));
    }
    transfer.upload->Unmap(0, nullptr);
    return transfer;
}

void record_upload(ID3D12GraphicsCommandList* commands, ID3D12Resource* texture, const TransferBuffers& transfer) {
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = transfer.mip;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = transfer.upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = transfer.footprint;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

void record_readback(ID3D12GraphicsCommandList* commands, ID3D12Resource* texture, const TransferBuffers& transfer) {
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = transfer.readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = transfer.footprint;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = transfer.mip;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

bool verify_readback(const TransferBuffers& transfer, std::uint8_t expected) {
    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(transfer.total_bytes)};
    if (FAILED(transfer.readback->Map(0, &read_range, &mapped))) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(mapped) + transfer.footprint.Offset;
    bool valid = true;
    for (UINT row = 0; row < transfer.rows && valid; ++row) {
        const auto* row_start = bytes + static_cast<std::size_t>(row) * transfer.footprint.Footprint.RowPitch;
        for (UINT64 column = 0; column < transfer.row_bytes; ++column) {
            if (row_start[column] != expected) {
                valid = false;
                break;
            }
        }
    }
    D3D12_RANGE empty_range{0, 0};
    transfer.readback->Unmap(0, &empty_range);
    return valid;
}

std::uint64_t debug_error_count(ID3D12InfoQueue* queue) {
    if (queue == nullptr) {
        return 0;
    }
    std::uint64_t errors = 0;
    const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T bytes = 0;
        if (FAILED(queue->GetMessage(index, nullptr, &bytes)) || bytes == 0) {
            continue;
        }
        std::vector<std::uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(queue->GetMessage(index, message, &bytes))) {
            continue;
        }
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
            message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            ++errors;
        }
    }
    return errors;
}

}  // namespace

int main() try {
    const bool debug_enabled = std::getenv("ARC_D3D12_DEBUG") != nullptr;
    if (debug_enabled) {
        ComPtr<ID3D12Debug> debug;
        check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12GetDebugInterface");
        debug->EnableDebugLayer();
    }

    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)), "CheckFeatureSupport");
    if (options.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED) {
        std::cout << "SKIP: tiled resources are not supported\n";
        return 77;
    }

    ComPtr<ID3D12InfoQueue> info_queue;
    if (debug_enabled) {
        check(device.As(&info_queue), "Query ID3D12InfoQueue");
    }

    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter3> adapter;
    check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid");

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence_event == nullptr) {
        throw std::runtime_error("CreateEventW failed");
    }
    std::uint64_t fence_value = 0;
    auto wait_queue = [&] {
        check(queue->Signal(fence.Get(), ++fence_value), "Queue Signal");
        check(fence->SetEventOnCompletion(fence_value, fence_event), "SetEventOnCompletion");
        if (WaitForSingleObject(fence_event, 30000) != WAIT_OBJECT_0) {
            throw std::runtime_error("GPU timeout");
        }
    };

    const UINT mip_levels = mip_count(kTextureWidth, kTextureHeight);
    D3D12_RESOURCE_DESC texture_description{};
    texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_description.Width = kTextureWidth;
    texture_description.Height = kTextureHeight;
    texture_description.DepthOrArraySize = 1;
    texture_description.MipLevels = static_cast<UINT16>(mip_levels);
    texture_description.Format = kTextureFormat;
    texture_description.SampleDesc.Count = 1;
    texture_description.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;

    const auto before = arc::dx12::query_memory_budget(adapter.Get());
    if (!before) {
        throw std::runtime_error("DXGI memory budget unavailable");
    }

    ComPtr<ID3D12Resource> texture;
    check(device->CreateReservedResource(
        &texture_description,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&texture)), "CreateReservedResource");

    UINT total_tiles = 0;
    D3D12_PACKED_MIP_INFO packed{};
    D3D12_TILE_SHAPE tile_shape{};
    UINT tiling_count = mip_levels;
    std::vector<D3D12_SUBRESOURCE_TILING> tilings(tiling_count);
    device->GetResourceTiling(
        texture.Get(),
        &total_tiles,
        &packed,
        &tile_shape,
        &tiling_count,
        0,
        tilings.data());
    if (packed.NumStandardMips < 3 || total_tiles == 0) {
        CloseHandle(fence_event);
        std::cout << "SKIP: insufficient standard mips for controlled test\n";
        return 77;
    }

    std::vector<TileRegion> regions;
    regions.reserve(packed.NumStandardMips + (packed.NumPackedMips > 0 ? 1U : 0U));
    UINT accounted_tiles = 0;
    for (UINT mip = 0; mip < packed.NumStandardMips; ++mip) {
        TileRegion region{};
        region.first_subresource = mip;
        region.coordinate = {0, 0, 0, mip};
        region.size.UseBox = TRUE;
        region.size.Width = tilings[mip].WidthInTiles;
        region.size.Height = tilings[mip].HeightInTiles;
        region.size.Depth = tilings[mip].DepthInTiles;
        region.tile_count = region.size.Width * region.size.Height * region.size.Depth;
        region.size.NumTiles = region.tile_count;
        region.heap = create_tile_heap(device.Get(), region.tile_count);
        accounted_tiles += region.tile_count;
        regions.push_back(std::move(region));
    }
    if (packed.NumPackedMips > 0) {
        TileRegion region{};
        region.first_subresource = packed.NumStandardMips;
        region.coordinate = {0, 0, 0, packed.NumStandardMips};
        region.size.UseBox = FALSE;
        region.size.NumTiles = packed.NumTilesForPackedMips;
        region.tile_count = packed.NumTilesForPackedMips;
        region.packed = true;
        region.heap = create_tile_heap(device.Get(), region.tile_count);
        accounted_tiles += region.tile_count;
        regions.push_back(std::move(region));
    }
    if (accounted_tiles != total_tiles) {
        throw std::runtime_error("tile accounting mismatch");
    }

    auto update_mapping = [&](TileRegion& region, bool map) {
        const D3D12_TILE_RANGE_FLAGS range_flag = map ? D3D12_TILE_RANGE_FLAG_NONE : D3D12_TILE_RANGE_FLAG_NULL;
        const UINT heap_offset = 0;
        const UINT tile_count = region.tile_count;
        queue->UpdateTileMappings(
            texture.Get(),
            1,
            &region.coordinate,
            &region.size,
            map ? region.heap.Get() : nullptr,
            1,
            &range_flag,
            &heap_offset,
            &tile_count,
            D3D12_TILE_MAPPING_FLAG_NONE);
    };

    for (auto& region : regions) {
        update_mapping(region, true);
    }
    wait_queue();

    std::array<TransferBuffers, 3> transfers{
        create_transfer(device.Get(), texture_description, 0, 0x11),
        create_transfer(device.Get(), texture_description, 1, 0x22),
        create_transfer(device.Get(), texture_description, 2, 0x44),
    };

    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList> commands;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "CreateCommandList");

    auto execute_commands = [&] {
        check(commands->Close(), "CommandList Close");
        ID3D12CommandList* lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        wait_queue();
    };
    auto reset_commands = [&] {
        check(allocator->Reset(), "CommandAllocator Reset");
        check(commands->Reset(allocator.Get(), nullptr), "CommandList Reset");
    };
    auto transition = [&](D3D12_RESOURCE_STATES before_state, D3D12_RESOURCE_STATES after_state) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before_state;
        barrier.Transition.StateAfter = after_state;
        commands->ResourceBarrier(1, &barrier);
    };

    for (const auto& transfer : transfers) {
        record_upload(commands.Get(), texture.Get(), transfer);
    }
    transition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (const auto& transfer : transfers) {
        record_readback(commands.Get(), texture.Get(), transfer);
    }
    execute_commands();

    bool initial_contents = true;
    for (const auto& transfer : transfers) {
        initial_contents = initial_contents && verify_readback(transfer, transfer.pattern);
    }
    if (!initial_contents) {
        throw std::runtime_error("initial mapped mip verification failed");
    }

    const auto fully_mapped = arc::dx12::query_memory_budget(adapter.Get());
    if (!fully_mapped) {
        throw std::runtime_error("DXGI memory budget unavailable after mapping");
    }

    arc::TextureQualityPolicyConfig quality_config{};
    quality_config.minimum_change_age_epochs = 0;
    arc::TextureQualityGovernor quality(quality_config);
    const std::uint64_t total_heap_bytes = static_cast<std::uint64_t>(total_tiles) * kTileBytes;
    const std::uint64_t mip0_bytes = static_cast<std::uint64_t>(regions[0].tile_count) * kTileBytes;
    const std::uint64_t mip1_bytes = static_cast<std::uint64_t>(regions[1].tile_count) * kTileBytes;
    arc::TextureQualityObject quality_object{};
    quality_object.id = 1;
    quality_object.resource = 1;
    quality_object.safety = arc::TextureQualitySafety::MipSafe;
    quality_object.full_resident_bytes = total_heap_bytes;
    quality_object.demotion_steps = {{mip0_bytes, 0.05}, {mip1_bytes, 0.15}};
    if (!quality.register_texture(quality_object)) {
        throw std::runtime_error("texture quality registration failed");
    }

    const auto demotions = quality.plan_demotions(mip0_bytes + mip1_bytes, 1);
    if (demotions.size() != 2 || demotions[0].from_level != 0 || demotions[1].from_level != 1) {
        throw std::runtime_error("texture quality governor did not plan sequential mip demotion");
    }
    for (const auto& action : demotions) {
        const UINT mip = action.from_level;
        update_mapping(regions[mip], false);
    }
    wait_queue();
    for (const auto& action : demotions) {
        const UINT mip = action.from_level;
        regions[mip].heap.Reset();
        if (!quality.apply(action, 1)) {
            throw std::runtime_error("failed to apply texture demotion state");
        }
    }

    std::uint64_t minimum_usage_after_drop = fully_mapped->local_usage;
    for (int sample = 0; sample < 8; ++sample) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        const auto budget = arc::dx12::query_memory_budget(adapter.Get());
        if (budget) {
            minimum_usage_after_drop = (std::min)(minimum_usage_after_drop, budget->local_usage);
        }
    }

    reset_commands();
    record_readback(commands.Get(), texture.Get(), transfers[2]);
    execute_commands();
    const bool lower_mip_preserved = verify_readback(transfers[2], transfers[2].pattern);
    if (!lower_mip_preserved) {
        throw std::runtime_error("lower mip content changed after top-mip unmapping");
    }

    const auto promotions = quality.plan_promotions(mip0_bytes + mip1_bytes, 2);
    if (promotions.size() != 2 || promotions[0].from_level != 2 || promotions[1].from_level != 1) {
        throw std::runtime_error("texture quality governor did not plan reverse mip promotion");
    }
    for (const auto& action : promotions) {
        const UINT mip = action.to_level;
        regions[mip].heap = create_tile_heap(device.Get(), regions[mip].tile_count);
        update_mapping(regions[mip], true);
    }
    wait_queue();

    transfers[0].pattern = 0x31;
    transfers[1].pattern = 0x32;
    for (UINT index = 0; index < 2; ++index) {
        void* mapped = nullptr;
        D3D12_RANGE empty_range{0, 0};
        check(transfers[index].upload->Map(0, &empty_range, &mapped), "Remap upload buffer");
        auto* bytes = static_cast<std::uint8_t*>(mapped) + transfers[index].footprint.Offset;
        for (UINT row = 0; row < transfers[index].rows; ++row) {
            std::memset(
                bytes + static_cast<std::size_t>(row) * transfers[index].footprint.Footprint.RowPitch,
                transfers[index].pattern,
                static_cast<std::size_t>(transfers[index].row_bytes));
        }
        transfers[index].upload->Unmap(0, nullptr);
    }

    reset_commands();
    transition(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    record_upload(commands.Get(), texture.Get(), transfers[0]);
    record_upload(commands.Get(), texture.Get(), transfers[1]);
    transition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    record_readback(commands.Get(), texture.Get(), transfers[0]);
    record_readback(commands.Get(), texture.Get(), transfers[1]);
    record_readback(commands.Get(), texture.Get(), transfers[2]);
    execute_commands();

    bool promotion_contents = verify_readback(transfers[0], transfers[0].pattern) &&
        verify_readback(transfers[1], transfers[1].pattern) &&
        verify_readback(transfers[2], transfers[2].pattern);
    if (!promotion_contents) {
        throw std::runtime_error("promoted mip verification failed");
    }
    for (const auto& action : promotions) {
        if (!quality.apply(action, 2)) {
            throw std::runtime_error("failed to apply texture promotion state");
        }
    }

    const auto restored = arc::dx12::query_memory_budget(adapter.Get());
    if (!restored) {
        throw std::runtime_error("DXGI memory budget unavailable after promotion");
    }

    const std::uint64_t observed_drop = fully_mapped->local_usage > minimum_usage_after_drop
        ? fully_mapped->local_usage - minimum_usage_after_drop
        : 0;
    const std::uint64_t expected_drop = mip0_bytes + mip1_bytes;
    const std::uint64_t debug_errors = debug_error_count(info_queue.Get());

    std::filesystem::create_directories("traces");
    std::ofstream output("traces/tiled-texture-lab.json", std::ios::trunc);
    output << "{\n"
           << "  \"tiled_resources_tier\": " << static_cast<unsigned>(options.TiledResourcesTier) << ",\n"
           << "  \"texture_width\": " << kTextureWidth << ",\n"
           << "  \"texture_height\": " << kTextureHeight << ",\n"
           << "  \"mip_levels\": " << mip_levels << ",\n"
           << "  \"standard_mips\": " << static_cast<unsigned>(packed.NumStandardMips) << ",\n"
           << "  \"packed_mips\": " << static_cast<unsigned>(packed.NumPackedMips) << ",\n"
           << "  \"total_tiles\": " << total_tiles << ",\n"
           << "  \"total_tile_heap_bytes\": " << total_heap_bytes << ",\n"
           << "  \"demoted_mip0_bytes\": " << mip0_bytes << ",\n"
           << "  \"demoted_mip1_bytes\": " << mip1_bytes << ",\n"
           << "  \"expected_drop_bytes\": " << expected_drop << ",\n"
           << "  \"dxgi_budget_bytes\": " << fully_mapped->local_budget << ",\n"
           << "  \"dxgi_usage_before_bytes\": " << before->local_usage << ",\n"
           << "  \"dxgi_usage_fully_mapped_bytes\": " << fully_mapped->local_usage << ",\n"
           << "  \"dxgi_usage_after_drop_min_bytes\": " << minimum_usage_after_drop << ",\n"
           << "  \"dxgi_observed_drop_bytes\": " << observed_drop << ",\n"
           << "  \"dxgi_usage_restored_bytes\": " << restored->local_usage << ",\n"
           << "  \"initial_contents_verified\": " << (initial_contents ? "true" : "false") << ",\n"
           << "  \"lower_mip_preserved\": " << (lower_mip_preserved ? "true" : "false") << ",\n"
           << "  \"promotion_contents_verified\": " << (promotion_contents ? "true" : "false") << ",\n"
           << "  \"debug_error_count\": " << debug_errors << "\n"
           << "}\n";
    output.close();

    std::cout << "Tiled texture lab: expected freed " << expected_drop
              << " bytes, DXGI observed " << observed_drop
              << " bytes, tiled tier " << static_cast<unsigned>(options.TiledResourcesTier)
              << ", debug errors " << debug_errors << '\n';

    CloseHandle(fence_event);
    if (debug_errors != 0) {
        return 1;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "tiled texture lab failed: " << error.what() << '\n';
    return 1;
}
