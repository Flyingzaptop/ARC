#include "generic_binding_state.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <limits>

namespace arc::dx12::binding {
namespace {
bool visible(D3D12_SHADER_VISIBILITY parameter, D3D12_SHADER_VISIBILITY stage) noexcept {
    return parameter == D3D12_SHADER_VISIBILITY_ALL || parameter == stage;
}
bool fits(UINT begin, UINT count) noexcept {
    return count && count != UINT_MAX && std::uint64_t(begin) + count <= std::uint64_t(UINT_MAX);
}
std::uint64_t mask(UINT count) noexcept {
    return count == 64 ? UINT64_MAX : ((std::uint64_t(1) << count) - 1);
}
}

Layout Layout::parse(std::span<const std::byte> bytes) {
    Layout out;
    auto reject = [&](const char* reason) { out.rejection = reason; return out; };
    if (bytes.empty() || bytes.size() > 1024 * 1024) return reject("root_blob_size");
    Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> decoder;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(bytes.data(), bytes.size(),
        IID_PPV_ARGS(&decoder)))) return reject("root_deserialization");
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* versioned{};
    if (FAILED(decoder->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &versioned)) ||
        !versioned) return reject("root_version");
    const auto& root = versioned->Desc_1_1;
    out.flags = root.Flags;
    if (root.NumParameters > 64 || root.NumStaticSamplers > 2048) return reject("root_capacity");
    // Direct heap indexing needs per-instruction dynamic index tracking. A table
    // inventory alone is not evidence that such a shader's bindings are known.
    if (root.Flags & (D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                      D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED))
        return reject("direct_heap_indexing");
    if (root.NumStaticSamplers)
        out.samplers.assign(root.pStaticSamplers, root.pStaticSamplers + root.NumStaticSamplers);
    std::size_t range_count{};
    for (UINT i = 0; i < root.NumParameters; ++i) {
        const auto& p = root.pParameters[i];
        Parameter item; item.type = p.ParameterType; item.visibility = p.ShaderVisibility;
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            ++out.dwords;
            const auto& table = p.DescriptorTable;
            range_count += table.NumDescriptorRanges;
            if (!table.NumDescriptorRanges || range_count > 4096) return reject("range_capacity");
            UINT next{};
            for (UINT j = 0; j < table.NumDescriptorRanges; ++j) {
                const auto& r = table.pDescriptorRanges[j];
                const UINT offset = r.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                    ? next : r.OffsetInDescriptorsFromTableStart;
                if (!fits(offset, r.NumDescriptors) || !fits(r.BaseShaderRegister, r.NumDescriptors))
                    return reject("unbounded_or_overflowing_range");
                item.ranges.push_back({r.RangeType, r.BaseShaderRegister, r.RegisterSpace,
                    r.NumDescriptors, offset, r.Flags});
                next = offset + r.NumDescriptors;
            }
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            item.shader_register = p.Constants.ShaderRegister; item.space = p.Constants.RegisterSpace;
            item.constants = p.Constants.Num32BitValues;
            if (!item.constants || item.constants > 64) return reject("constant_capacity");
            out.dwords += item.constants;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            item.shader_register = p.Descriptor.ShaderRegister; item.space = p.Descriptor.RegisterSpace;
            out.dwords += 2;
            break;
        default: return reject("unknown_root_parameter");
        }
        if (out.dwords > 64) return reject("root_dword_budget");
        out.parameters.push_back(std::move(item));
    }
    out.complete = true;
    return out;
}

std::optional<Location> Layout::locate(D3D12_DESCRIPTOR_RANGE_TYPE type,
    UINT reg, UINT space, D3D12_SHADER_VISIBILITY stage) const noexcept {
    if (!complete) return {};
    std::optional<Location> found;
    for (UINT i = 0; i < parameters.size(); ++i) {
        const auto& p = parameters[i];
        if (!visible(p.visibility, stage)) continue;
        if (p.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            for (const auto& r : p.ranges) {
                if (r.type != type || r.space != space || reg < r.first_register ||
                    reg - r.first_register >= r.count) continue;
                if (found) return {}; // Ambiguity cannot authorize a mutation.
                found = Location{i, r.table_offset + reg - r.first_register, true};
            }
        } else {
            const bool matches = (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV &&
                    (p.type == D3D12_ROOT_PARAMETER_TYPE_CBV || p.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)) ||
                (type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV && p.type == D3D12_ROOT_PARAMETER_TYPE_SRV) ||
                (type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV && p.type == D3D12_ROOT_PARAMETER_TYPE_UAV);
            if (!matches || p.shader_register != reg || p.space != space) continue;
            if (found) return {};
            found = Location{i, 0, false, p.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
        }
    }
    if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
        for (const auto& sampler : samplers) {
            if (sampler.ShaderRegister != reg || sampler.RegisterSpace != space || !visible(sampler.ShaderVisibility, stage)) continue;
            if (found) return {};
            Location l; l.static_sampler = true; l.sampler = sampler; found = l;
        }
    }
    return found;
}

std::vector<std::byte> append_control_cbv(std::span<const std::byte> original,UINT space) {
    const auto parsed=Layout::parse(original);
    if(!parsed.complete||parsed.dwords>62||parsed.parameters.size()>=64)return {};
    for(const auto& p:parsed.parameters){
        if(p.type==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){
            for(const auto& r:p.ranges)if(r.type==D3D12_DESCRIPTOR_RANGE_TYPE_CBV&&r.space==space&&r.first_register==0)return {};
        }else if((p.type==D3D12_ROOT_PARAMETER_TYPE_CBV||p.type==D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)&&p.space==space&&p.shader_register==0)return {};
    }
    Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> decoder;
    if(FAILED(D3D12CreateVersionedRootSignatureDeserializer(original.data(),original.size(),IID_PPV_ARGS(&decoder))))return {};
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* description{};
    if(FAILED(decoder->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1,&description)))return {};
    auto augmented=*description;std::vector<D3D12_ROOT_PARAMETER1> parameters;
    if(augmented.Desc_1_1.NumParameters)parameters.assign(augmented.Desc_1_1.pParameters,augmented.Desc_1_1.pParameters+augmented.Desc_1_1.NumParameters);
    D3D12_ROOT_PARAMETER1 control{};control.ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    control.Descriptor={0,space,D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};control.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    parameters.push_back(control);augmented.Desc_1_1.NumParameters=static_cast<UINT>(parameters.size());augmented.Desc_1_1.pParameters=parameters.data();
    Microsoft::WRL::ComPtr<ID3DBlob> blob,error;
    if(FAILED(D3D12SerializeVersionedRootSignature(&augmented,&blob,&error)))return {};
    const auto* begin=static_cast<const std::byte*>(blob->GetBufferPointer());return {begin,begin+blob->GetBufferSize()};
}

void Arguments::reset() noexcept { identity_ = 0; layout_.reset(); arguments_ = {}; }
void Arguments::signature(std::uint64_t identity, std::shared_ptr<const Layout> layout) {
    if (identity && identity == identity_) return;
    reset(); identity_ = identity; layout_ = std::move(layout);
    if (!identity_ || !layout_ || !layout_->complete) return;
    for (UINT i = 0; i < layout_->parameters.size(); ++i) arguments_[i].type = layout_->parameters[i].type;
}
bool Arguments::table(UINT parameter, D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept {
    if (!layout_ || !layout_->complete || parameter >= layout_->parameters.size() ||
        arguments_[parameter].type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) return false;
    auto& a = arguments_[parameter]; a.address = handle.ptr; a.initialized = handle.ptr != 0; return a.initialized;
}
bool Arguments::descriptor(UINT parameter, D3D12_ROOT_PARAMETER_TYPE type, UINT64 address) noexcept {
    if (type != D3D12_ROOT_PARAMETER_TYPE_CBV && type != D3D12_ROOT_PARAMETER_TYPE_SRV &&
        type != D3D12_ROOT_PARAMETER_TYPE_UAV) return false;
    if (!layout_ || !layout_->complete || parameter >= layout_->parameters.size() || arguments_[parameter].type != type) return false;
    auto& a = arguments_[parameter]; a.address = address; a.initialized = address != 0; return a.initialized;
}
bool Arguments::constants(UINT parameter, UINT offset, std::span<const UINT> words) noexcept {
    if (!layout_ || !layout_->complete || parameter >= layout_->parameters.size() ||
        arguments_[parameter].type != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) return false;
    const UINT count = layout_->parameters[parameter].constants;
    if (offset > count || words.size() > count - offset) return false;
    auto& a = arguments_[parameter];
    std::copy(words.begin(), words.end(), a.words.begin() + offset);
    if (!words.empty()) a.written |= mask(static_cast<UINT>(words.size())) << offset;
    a.initialized = a.written == mask(count);
    return true;
}
void Arguments::invalidate_tables() noexcept {
    for (auto& a : arguments_) if (a.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) a.initialized = false;
}
std::optional<Argument> Arguments::argument(UINT parameter) const noexcept {
    if (!layout_ || !layout_->complete || parameter >= layout_->parameters.size() || !arguments_[parameter].initialized) return {};
    return arguments_[parameter];
}
std::optional<Argument> Arguments::raw_argument(UINT parameter) const noexcept {
    if(!layout_||!layout_->complete||parameter>=layout_->parameters.size())return {};
    return arguments_[parameter];
}
std::optional<Location> Arguments::locate(D3D12_DESCRIPTOR_RANGE_TYPE type,
    UINT reg, UINT space, D3D12_SHADER_VISIBILITY stage) const noexcept {
    if (!layout_) return {};
    auto location = layout_->locate(type, reg, space, stage);
    if (location && !location->static_sampler && !argument(location->parameter)) return {};
    return location;
}
} // namespace arc::dx12::binding
