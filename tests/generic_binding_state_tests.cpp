#include "generic_binding_state.hpp"
#include "generic_binding_admission.hpp"
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
    const std::span<const std::byte> bytes{static_cast<const std::byte*>(blob->GetBufferPointer()), blob->GetBufferSize()};
    if(!unbounded){const auto extended=append_control_cbv(bytes,13);assert(!extended.empty());const auto check=Layout::parse(extended);assert(check.complete&&check.dwords==9&&check.parameters.size()==4);assert(check.locate(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,0,13,D3D12_SHADER_VISIBILITY_ALL)->parameter==3);}
    return std::make_shared<Layout>(Layout::parse(bytes));
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
    // Bindings are resolved from the current descriptor contents at submission,
    // not from values cached when a command was recorded.
    Arguments submitted;submitted.signature(7,root);assert(submitted.table(0,{0x10000}));
    arc::DescriptorLedger ledger;assert(ledger.register_heap(1,0x1000,32,32));
    std::vector<DescriptorHeap> heaps{{1,0x1000,0x10000,32,32,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}};
    std::map<std::uint64_t,Allocation> allocations;
    D3D12_RESOURCE_DESC image{};image.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;image.Width=61;image.Height=37;
    image.DepthOrArraySize=image.MipLevels=image.SampleDesc.Count=1;image.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    for(UINT id=1;id<=5;++id)allocations[id]={id,0,0,65536,0,AllocationKind::Committed,image};
    auto view=[&](UINT id,UINT kind){arc::DescriptorValue v{id,kind,0,1};v.shape.known=true;v.shape.format=image.Format;
        v.shape.dimension=kind==1?static_cast<UINT>(D3D12_SRV_DIMENSION_TEXTURE2D):static_cast<UINT>(D3D12_UAV_DIMENSION_TEXTURE2D);return v;};
    for(UINT i=0;i<3;++i)assert(ledger.write(0x1000+(4+i)*32,view(i+1,1)));
    assert(ledger.write(0x1000+7*32,view(4,2)));assert(ledger.write(0x1000+8*32,view(5,2)));
    arc::dx12::shader::Transform transform;transform.admitted=true;transform.threads={8,8,1};
    transform.resources={{0,0,7,2,3,2},{1,0,5,3,1,2},{1,1,6,3,1,2}};
    auto admit=[&]{return admit_compute(submitted,transform,ledger,heaps,allocations,8,5,1);};
    assert(admit().admitted);
    assert(ledger.write(0x1000+7*32,view(1,2)));
    assert(!admit().admitted&&admit().reason=="input_output_alias");
    assert(ledger.write(0x1000+7*32,view(4,2)));
    allocations[1].kind=allocations[4].kind=AllocationKind::Placed;
    allocations[1].heap=allocations[4].heap=99;allocations[4].offset=32768;
    assert(!admit().admitted&&admit().reason=="input_output_alias");
    allocations[4].offset=65536;assert(admit().admitted);
    allocations[4].kind=AllocationKind::Reserved;assert(!admit().admitted);
    allocations[4].kind=AllocationKind::Placed;
    auto incomplete=view(4,2);incomplete.shape.known=false;assert(ledger.write(0x1000+7*32,incomplete));assert(!admit().admitted);
    assert(ledger.write(0x1000+7*32,view(4,2)));assert(admit().admitted);
    assert(!admit_compute(submitted,transform,ledger,heaps,allocations,7,5,1).admitted);
    assert(!admit_compute(submitted,transform,ledger,heaps,allocations,8,5,2).admitted);
    assert(ledger.write(0x1000+4*32,view(0,1)));assert(admit().admitted); // explicit null SRV is known
    submitted.invalidate_tables();assert(!admit().admitted);
    std::cout << "Root ranges, spaces, APPEND, visibility, constants and invalidation passed\n";
}
