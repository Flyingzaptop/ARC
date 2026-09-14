#include "arc/dx12_observer.hpp"
namespace arc::dx12 {
bool Observer::observe_barrier(CommandId command, ResourceId resource, const D3D12_RESOURCE_BARRIER& b, ResourceId related) noexcept {
    if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        return observe(EventType::Barrier, BarrierPayload{.resource = resource, .command = command, .before_state = static_cast<unsigned>(b.Transition.StateBefore), .after_state = static_cast<unsigned>(b.Transition.StateAfter), .subresource = b.Transition.Subresource, .reserved = static_cast<unsigned>(b.Flags)});
    }
    return observe(EventType::ExtendedBarrier, ExtendedBarrierPayload{.command = command, .resource = resource, .related_resource = related, .flags = static_cast<unsigned>(b.Flags), .kind = b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING ? 1U : 2U});
}
bool Observer::observe_barrier(CommandId command, const D3D12_GLOBAL_BARRIER& b) noexcept {
    return observe(EventType::ExtendedBarrier, ExtendedBarrierPayload{.command = command, .sync_before = static_cast<std::uint64_t>(b.SyncBefore), .sync_after = static_cast<std::uint64_t>(b.SyncAfter), .access_before = static_cast<std::uint64_t>(b.AccessBefore), .access_after = static_cast<std::uint64_t>(b.AccessAfter), .kind = 3});
}
bool Observer::observe_barrier(CommandId command, ResourceId resource, const D3D12_BUFFER_BARRIER& b) noexcept {
    return observe(EventType::ExtendedBarrier, ExtendedBarrierPayload{.command = command, .resource = resource, .sync_before = static_cast<std::uint64_t>(b.SyncBefore), .sync_after = static_cast<std::uint64_t>(b.SyncAfter), .access_before = static_cast<std::uint64_t>(b.AccessBefore), .access_after = static_cast<std::uint64_t>(b.AccessAfter), .offset = b.Offset, .bytes = b.Size, .kind = 4});
}
bool Observer::observe_barrier(CommandId command, ResourceId resource, const D3D12_TEXTURE_BARRIER& b) noexcept {
    return observe(EventType::ExtendedBarrier, ExtendedBarrierPayload{.command = command, .resource = resource, .sync_before = static_cast<std::uint64_t>(b.SyncBefore), .sync_after = static_cast<std::uint64_t>(b.SyncAfter), .access_before = static_cast<std::uint64_t>(b.AccessBefore), .access_after = static_cast<std::uint64_t>(b.AccessAfter), .layout_before = static_cast<unsigned>(b.LayoutBefore), .layout_after = static_cast<unsigned>(b.LayoutAfter), .first_mip = b.Subresources.IndexOrFirstMipLevel, .mip_count = b.Subresources.NumMipLevels, .first_layer = b.Subresources.FirstArraySlice, .layer_count = b.Subresources.NumArraySlices, .first_plane = b.Subresources.FirstPlane, .plane_count = b.Subresources.NumPlanes, .flags = static_cast<unsigned>(b.Flags), .kind = 5});
}
}
