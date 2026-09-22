#pragma once
#include <d3d12.h>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>
namespace arc::dx12 {
struct ShadowBufferInput {ID3D12Resource* resource{};UINT64 offset{},bytes{};};
struct ShadowInputSnapshot {
    std::vector<std::byte> bytes;
    bool complete{};
    const char* reason{"unknown"};
};
// Capture at the submission boundary, not at command recording. The caller
// owns resources and must separately prove complete shader dependency coverage.
// GPU-written/default-heap buffers are deliberately not CPU-read or guessed.
inline ShadowInputSnapshot snapshot_shadow_uploads(std::span<const ShadowBufferInput> inputs){
    ShadowInputSnapshot out;constexpr UINT64 limit=1024*1024;
    if(inputs.empty()||inputs.size()>64){out.reason="input_count";return out;}
    UINT64 total=0;
    for(const auto& input:inputs){
        if(!input.resource||!input.bytes){out.reason="missing_input";return out;}
        const auto d=input.resource->GetDesc();D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||FAILED(input.resource->GetHeapProperties(&heap,&flags))||heap.Type!=D3D12_HEAP_TYPE_UPLOAD){out.reason="gpu_or_unknown_content";return out;}
        if(input.offset>d.Width||input.bytes>d.Width-input.offset||input.bytes>limit-total){out.reason="range_or_capacity";return out;}
        total+=input.bytes;
    }
    out.bytes.reserve(static_cast<size_t>(total)+inputs.size()*sizeof(UINT64));
    for(const auto& input:inputs){
        void* data{};D3D12_RANGE range{static_cast<SIZE_T>(input.offset),static_cast<SIZE_T>(input.offset+input.bytes)};
        if(FAILED(input.resource->Map(0,&range,&data))||!data){out.bytes.clear();out.reason="map_failed";return out;}
        const auto start=out.bytes.size();out.bytes.resize(start+sizeof(UINT64)+static_cast<size_t>(input.bytes));
        std::memcpy(out.bytes.data()+start,&input.bytes,sizeof(UINT64));
        std::memcpy(out.bytes.data()+start+sizeof(UINT64),static_cast<const std::byte*>(data)+input.offset,static_cast<size_t>(input.bytes));
        D3D12_RANGE written{0,0};input.resource->Unmap(0,&written);
    }
    out.complete=true;out.reason="upload_bytes_captured";return out;
}
}
