#include "generic_binding_state.hpp"
#include <wrl/client.h>
#include <cassert>
#include <iostream>

using namespace arc::dx12::binding;
using Microsoft::WRL::ComPtr;

std::shared_ptr<Layout> make_layout(bool unbounded = false) {
    D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 7, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 4};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, unbounded ? UINT_MAX : 2, 5, 3,
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER1 parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {2, ranges};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {9, 4, 4};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[2].Descriptor = {10, 4, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderRegister = 2; sampler.RegisterSpace = 8;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{}; desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1 = {3, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    assert(SUCCEEDED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error)));
    return std::make_shared<Layout>(Layout::parse({static_cast<const std::byte*>(blob->GetBufferPointer()), blob->GetBufferSize()}));
}

int main() {
    assert(!Layout::parse({}).complete);
    const std::byte invalid[]{std::byte{0}, std::byte{1}};
    assert(!Layout::parse(invalid).complete);
    auto root = make_layout(); assert(root->complete && root->dwords == 7);
    const auto srv = root->locate(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 9, 2, D3D12_SHADER_VISIBILITY_ALL);
    assert(srv && srv->table && srv->table_offset == 6);
    const auto uav = root->locate(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 3, D3D12_SHADER_VISIBILITY_ALL);
    assert(uav && uav->table_offset == 8);
    assert(!root->locate(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 2, D3D12_SHADER_VISIBILITY_ALL));
    assert(!root->locate(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 2, D3D12_SHADER_VISIBILITY_ALL));
    assert(root->locate(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2, 8, D3D12_SHADER_VISIBILITY_PIXEL)->static_sampler);
    assert(!root->locate(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2, 8, D3D12_SHADER_VISIBILITY_VERTEX));
    assert(!make_layout(true)->complete);

    Arguments state; state.signature(1, root);
    assert(!state.locate(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 2, D3D12_SHADER_VISIBILITY_ALL));
    assert(state.table(0, {0x1000}));
    assert(state.locate(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 2, D3D12_SHADER_VISIBILITY_ALL));
    assert(!state.descriptor(0, D3D12_ROOT_PARAMETER_TYPE_CBV, 0x2000));
    assert(state.descriptor(2, D3D12_ROOT_PARAMETER_TYPE_CBV, 0x2000));
    const UINT first[]{11, 12}, last[]{13, 14};
    assert(state.constants(1, 2, last)); assert(!state.argument(1));
    assert(state.constants(1, 0, first)); assert(state.argument(1)->words[3] == 14);
    assert(!state.constants(1, UINT_MAX, first));
    assert(!state.constants(1, 3, first));
    state.signature(1, root); assert(state.argument(1)); // same signature preserves state
    state.invalidate_tables(); assert(!state.argument(0)); assert(state.argument(1)); assert(state.argument(2));
    state.signature(2, root); assert(!state.argument(1)); assert(!state.argument(2));
    state.reset(); assert(!state.locate(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2, 8, D3D12_SHADER_VISIBILITY_PIXEL));

    auto wide = std::make_shared<Layout>(); wide->complete = true;
    Parameter p; p.type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p.constants = 64;
    wide->parameters.push_back(p); state.signature(3, wide);
    UINT words[64]{}; words[63] = 123;
    assert(state.constants(0, 0, words)); assert(state.argument(0)->words[63] == 123);
    assert(state.constants(0, 64, {}));
    std::cout << "Root ranges, spaces, APPEND, visibility, constants and invalidation passed\n";
}
