#pragma once
#include "generic_binding_state.hpp"
#include "generic_shader_transform.hpp"
#include "arc/descriptor_ledger.hpp"
#include <map>
#include <string>

namespace arc::dx12::binding {
enum class AllocationKind { Unknown, Committed, Placed, Reserved };
struct Allocation {
    std::uint64_t id{}, heap{}, offset{}, bytes{}, gpu_address{};
    AllocationKind kind{};
    D3D12_RESOURCE_DESC description{};
};
struct DescriptorHeap {
    std::uint64_t id{}, cpu{}, gpu{};
    UINT stride{}, count{};
    D3D12_DESCRIPTOR_HEAP_TYPE type{};
};
struct BoundResource {
    shader::ResourceContract contract;
    DescriptorValue view;
    Allocation allocation;
};
struct Admission {
    bool admitted{};
    std::string reason;
    UINT width{},height{};
    UINT binding_class{},binding_register{},binding_space{};
    std::vector<BoundResource> inputs,outputs;
};

// Resolve at SUBMISSION, including every replay, while descriptor-write hooks
// are serialized with the native submission. Root Signature 1.0/volatile
// descriptors may legitimately change after recording and before submission.
Admission admit_compute(const Arguments&, const shader::Transform&,
    const DescriptorLedger&, const std::vector<DescriptorHeap>& bound_heaps,
    const std::map<std::uint64_t,Allocation>& allocations,
    UINT groups_x, UINT groups_y, UINT groups_z);
}
