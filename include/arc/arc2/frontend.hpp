#pragma once

#include <d3d12.h>
#include <cstdint>

namespace arc::arc2 {
class Runtime;
Runtime& frontend_runtime() noexcept;
struct DescriptorRef;
DescriptorRef resolve_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept;

// Borrowed native pointer. The caller must keep its ARC wrapper alive.
IUnknown* unwrap_unknown(IUnknown* object) noexcept;
std::uint64_t object_id(IUnknown* object) noexcept;

// Consumes one native reference, including on failure. Never returns an
// unwrapped D3D12 object to the application.
HRESULT wrap_object(IUnknown* native, REFIID iid, void** out) noexcept;
inline HRESULT wrap_resource(IUnknown* native, REFIID iid, void** out) noexcept {
    return wrap_object(native, iid, out);
}

HRESULT create_device(IUnknown* adapter, D3D_FEATURE_LEVEL level,
                      REFIID iid, void** out) noexcept;

using ClearSkipAdmit = bool (*)(std::uint64_t list, bool redundant) noexcept;
void set_clear_skip_admit(ClearSkipAdmit callback) noexcept;

} // namespace arc::arc2
