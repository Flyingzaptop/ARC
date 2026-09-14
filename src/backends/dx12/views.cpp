#include "arc/dx12_observer.hpp"
namespace arc::dx12 {
std::uint64_t Observer::observe_descriptor_heap(const D3D12_DESCRIPTOR_HEAP_DESC& d, std::uint32_t increment) noexcept {
    const auto id = ids_.next();
    return observe(EventType::DescriptorHeapCreated, DescriptorHeapPayload{.heap = id, .count = d.NumDescriptors, .type = static_cast<unsigned>(d.Type), .increment = increment, .flags = static_cast<unsigned>(d.Flags)}) ? id : 0;
}
void Observer::observe_descriptor_heap_destroyed(std::uint64_t heap) noexcept {
    (void)observe(EventType::DescriptorHeapDestroyed, DescriptorHeapPayload{.heap = heap});
}
bool Observer::observe_srv(DescriptorId id, ResourceId resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& v) noexcept {
    DescriptorWrittenPayload p{.descriptor = id, .resource = resource, .type = ViewType::Srv, .mip_count = 1, .layer_count = 1, .format = static_cast<unsigned>(v.Format)};
    switch (v.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE2D: p.first_mip = static_cast<std::uint32_t>(v.Texture2D.MostDetailedMip); p.mip_count = static_cast<std::uint32_t>(v.Texture2D.MipLevels); break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: p.first_mip = static_cast<std::uint32_t>(v.Texture2DArray.MostDetailedMip); p.mip_count = static_cast<std::uint32_t>(v.Texture2DArray.MipLevels); p.first_layer = static_cast<std::uint32_t>(v.Texture2DArray.FirstArraySlice); p.layer_count = static_cast<std::uint32_t>(v.Texture2DArray.ArraySize); break;
    case D3D12_SRV_DIMENSION_TEXTURE3D: p.first_mip = static_cast<std::uint32_t>(v.Texture3D.MostDetailedMip); p.mip_count = static_cast<std::uint32_t>(v.Texture3D.MipLevels); break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE: p.first_mip = static_cast<std::uint32_t>(v.TextureCube.MostDetailedMip); p.mip_count = static_cast<std::uint32_t>(v.TextureCube.MipLevels); p.layer_count = 6; break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: p.first_mip = static_cast<std::uint32_t>(v.TextureCubeArray.MostDetailedMip); p.mip_count = static_cast<std::uint32_t>(v.TextureCubeArray.MipLevels); p.first_layer = static_cast<std::uint32_t>(v.TextureCubeArray.First2DArrayFace); p.layer_count = static_cast<std::uint32_t>(v.TextureCubeArray.NumCubes * 6); break;
    case D3D12_SRV_DIMENSION_TEXTURE1D: p.first_mip = static_cast<std::uint32_t>(v.Texture1D.MostDetailedMip); p.mip_count = static_cast<std::uint32_t>(v.Texture1D.MipLevels); break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: p.first_mip = static_cast<std::uint32_t>(v.Texture1DArray.MostDetailedMip); p.mip_count = static_cast<std::uint32_t>(v.Texture1DArray.MipLevels); p.first_layer = static_cast<std::uint32_t>(v.Texture1DArray.FirstArraySlice); p.layer_count = static_cast<std::uint32_t>(v.Texture1DArray.ArraySize); break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS: break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: p.first_layer = static_cast<std::uint32_t>(v.Texture2DMSArray.FirstArraySlice); p.layer_count = static_cast<std::uint32_t>(v.Texture2DMSArray.ArraySize); break;
    case D3D12_SRV_DIMENSION_BUFFER: p.mip_count = p.layer_count = 0; p.buffer_offset = v.Buffer.FirstElement * v.Buffer.StructureByteStride; p.buffer_bytes = static_cast<std::uint64_t>(v.Buffer.NumElements) * v.Buffer.StructureByteStride; break;
    default: p.type = ViewType::Unknown; break;
    }
    return observe(EventType::DescriptorWritten, p);
}
bool Observer::observe_uav(DescriptorId id, ResourceId resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& v) noexcept {
    DescriptorWrittenPayload p{.descriptor = id, .resource = resource, .type = ViewType::Uav, .mip_count = 1, .layer_count = 1, .format = static_cast<unsigned>(v.Format)};
    switch (v.ViewDimension) {
    case D3D12_UAV_DIMENSION_TEXTURE2D: p.first_mip = static_cast<std::uint32_t>(v.Texture2D.MipSlice); break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY: p.first_mip = static_cast<std::uint32_t>(v.Texture2DArray.MipSlice); p.first_layer = static_cast<std::uint32_t>(v.Texture2DArray.FirstArraySlice); p.layer_count = static_cast<std::uint32_t>(v.Texture2DArray.ArraySize); break;
    case D3D12_UAV_DIMENSION_TEXTURE3D: p.first_mip = static_cast<std::uint32_t>(v.Texture3D.MipSlice); break;
    case D3D12_UAV_DIMENSION_TEXTURE1D: p.first_mip = static_cast<std::uint32_t>(v.Texture1D.MipSlice); break;
    case D3D12_UAV_DIMENSION_BUFFER: p.mip_count = p.layer_count = 0; p.buffer_offset = v.Buffer.FirstElement * v.Buffer.StructureByteStride; p.buffer_bytes = static_cast<std::uint64_t>(v.Buffer.NumElements) * v.Buffer.StructureByteStride; break;
    default: p.type = ViewType::Unknown; break;
    }
    return observe(EventType::DescriptorWritten, p);
}
bool Observer::observe_rtv(DescriptorId id, ResourceId resource, const D3D12_RENDER_TARGET_VIEW_DESC& v) noexcept {
    DescriptorWrittenPayload p{.descriptor = id, .resource = resource, .type = ViewType::Rtv, .mip_count = 1, .layer_count = 1, .format = static_cast<unsigned>(v.Format)};
    switch (v.ViewDimension) {
    case D3D12_RTV_DIMENSION_TEXTURE2D: p.first_mip = static_cast<std::uint32_t>(v.Texture2D.MipSlice); break;
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY: p.first_mip = static_cast<std::uint32_t>(v.Texture2DArray.MipSlice); p.first_layer = static_cast<std::uint32_t>(v.Texture2DArray.FirstArraySlice); p.layer_count = static_cast<std::uint32_t>(v.Texture2DArray.ArraySize); break;
    case D3D12_RTV_DIMENSION_TEXTURE2DMS: break;
    default: p.type = ViewType::Unknown; break;
    }
    return observe(EventType::DescriptorWritten, p);
}
bool Observer::observe_dsv(DescriptorId id, ResourceId resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& v) noexcept {
    DescriptorWrittenPayload p{.descriptor = id, .resource = resource, .type = ViewType::Dsv, .mip_count = 1, .layer_count = 1, .format = static_cast<unsigned>(v.Format)};
    switch (v.ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE2D: p.first_mip = static_cast<std::uint32_t>(v.Texture2D.MipSlice); break;
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY: p.first_mip = static_cast<std::uint32_t>(v.Texture2DArray.MipSlice); p.first_layer = static_cast<std::uint32_t>(v.Texture2DArray.FirstArraySlice); p.layer_count = static_cast<std::uint32_t>(v.Texture2DArray.ArraySize); break;
    case D3D12_DSV_DIMENSION_TEXTURE2DMS: break;
    default: p.type = ViewType::Unknown; break;
    }
    return observe(EventType::DescriptorWritten, p);
}
bool Observer::observe_cbv(DescriptorId id, ResourceId resource, std::uint64_t offset, std::uint32_t bytes) noexcept {
    return observe(EventType::DescriptorWritten, DescriptorWrittenPayload{.descriptor = id, .resource = resource, .type = ViewType::Cbv, .buffer_offset = offset, .buffer_bytes = bytes});
}
bool Observer::observe_sampler(DescriptorId id) noexcept {
    return observe(EventType::DescriptorWritten, DescriptorWrittenPayload{.descriptor = id, .type = ViewType::Sampler});
}
}
