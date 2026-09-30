#include "arc/arc2/frontend.hpp"
#include "arc/arc2/runtime.hpp"
#include "shader_semantics.hpp"
#include <atomic>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include <mutex>
#include <type_traits>
#include <unordered_map>

namespace arc::arc2 {
namespace {
std::mutex registry_mutex;
struct ProxyBase;
std::unordered_map<IUnknown*, ProxyBase*> by_native;
std::unordered_map<IUnknown*, ProxyBase*> by_wrapper;
std::atomic<ClearSkipAdmit> clear_admit{nullptr};
#include "frontend_debug.inc"
template<class T> T native_arg(T value) noexcept;
template<class T> T* native_arg(T* value) noexcept;
#include "frontend_ledger.inc"

IUnknown* identity(IUnknown* p) noexcept {
    if (!p) return nullptr;
    IUnknown* q = nullptr;
    if (FAILED(p->QueryInterface(IID_PPV_ARGS(&q)))) return nullptr;
    q->Release();
    return q;
}
std::string iid_name(REFIID iid) {
    wchar_t wide[40]{};
    if (!StringFromGUID2(iid, wide, 40)) return "unknown IID";
    char utf8[80]{};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, 80, nullptr, nullptr);
    return utf8;
}

template<class T> T native_arg(T value) noexcept { return value; }
template<class T> T* native_arg(T* value) noexcept {
    if constexpr (std::is_base_of_v<IUnknown, std::remove_cv_t<T>>) {
        return static_cast<T*>(unwrap_unknown(const_cast<std::remove_cv_t<T>*>(value)));
    } else return value;
}

struct ProxyBase {
    ProxyBase(IUnknown* native, ObjectId object) : native(native), object(object), native_identity(identity(native)) {}
    virtual ~ProxyBase() {
        { std::lock_guard lock(registry_mutex);
          descriptor_heaps.erase(std::remove_if(descriptor_heaps.begin(), descriptor_heaps.end(),
              [&](const HeapRange& h) { return h.id == object; }), descriptor_heaps.end());
          buffers.erase(std::remove_if(buffers.begin(), buffers.end(),
              [&](const BufferRange& b) { return b.id == object; }), buffers.end()); }
        frontend_runtime().destroy_object(object);
        native->Release();
    }
    virtual bool supports(REFIID) const noexcept = 0;
    virtual void* interface_ptr() noexcept = 0;
    virtual const char* type_name() const noexcept = 0;
    HRESULT set_private_bytes(REFGUID guid, UINT size, const void* data) noexcept {
        ID3D12Object* object_interface = nullptr;
        if (FAILED(native->QueryInterface(IID_PPV_ARGS(&object_interface)))) return E_NOINTERFACE;
        auto hr = object_interface->SetPrivateData(guid, size, data);
        object_interface->Release();
        if (SUCCEEDED(hr)) {
            std::lock_guard lock(private_mutex);
            private_interfaces.erase(std::remove_if(private_interfaces.begin(), private_interfaces.end(),
                [&](const GUID& known) { return IsEqualGUID(known, guid); }), private_interfaces.end());
        }
        return hr;
    }
    HRESULT set_private_interface(REFGUID guid, const IUnknown* data) noexcept {
        ID3D12Object* object_interface = nullptr;
        if (FAILED(native->QueryInterface(IID_PPV_ARGS(&object_interface)))) return E_NOINTERFACE;
        // Private interface data is opaque COM storage. Let the native object
        // retain the application's wrapper reference and its stable IR identity.
        auto hr = object_interface->SetPrivateDataInterface(guid, data);
        object_interface->Release();
        if (SUCCEEDED(hr)) {
            std::lock_guard lock(private_mutex);
            private_interfaces.erase(std::remove_if(private_interfaces.begin(), private_interfaces.end(),
                [&](const GUID& known) { return IsEqualGUID(known, guid); }), private_interfaces.end());
            if (data) private_interfaces.push_back(guid);
        }
        return hr;
    }
    HRESULT get_private_data(REFGUID guid, UINT* size, void* data) noexcept {
        ID3D12Object* object_interface = nullptr;
        if (FAILED(native->QueryInterface(IID_PPV_ARGS(&object_interface)))) return E_NOINTERFACE;
        auto hr = object_interface->GetPrivateData(guid, size, data);
        object_interface->Release();
        if (FAILED(hr) || !data || !size || *size != sizeof(IUnknown*)) return hr;
        bool stored_interface = false;
        { std::lock_guard lock(private_mutex);
          stored_interface = std::any_of(private_interfaces.begin(), private_interfaces.end(),
              [&](const GUID& known) { return IsEqualGUID(known, guid); }); }
        if (!stored_interface) return hr;
        auto* raw = *static_cast<IUnknown**>(data);
        if (!raw) return hr;
        { std::lock_guard lock(registry_mutex);
          if (by_wrapper.contains(raw)) return hr; }
        ID3D12Object* probe = nullptr;
        if (FAILED(raw->QueryInterface(IID_PPV_ARGS(&probe)))) return hr;
        probe->Release();
        void* wrapped = nullptr;
        auto wrapped_hr = wrap_object(raw, __uuidof(IUnknown), &wrapped);
        *static_cast<IUnknown**>(data) = static_cast<IUnknown*>(wrapped);
        return wrapped_hr;
    }
    HRESULT query(REFIID iid, void** out) noexcept {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!supports(iid)) {
            IUnknown* supported = nullptr;
            auto native_hr = native->QueryInterface(iid, reinterpret_cast<void**>(&supported));
            if (FAILED(native_hr)) return native_hr;
            if (supported) supported->Release();
            const auto name = "frontend missing native-supported IID " + iid_name(iid);
            debug_trace(name.c_str(), this, native, object.value);
            frontend_runtime().note_coverage(name);
            return E_NOINTERFACE;
        }
        add_ref();
        *out = interface_ptr();
        return S_OK;
    }
    ULONG add_ref() noexcept { return refs.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG release() noexcept {
        debug_trace("COM Release", this, native, object.value);
        ULONG left;
        {
            std::lock_guard lock(registry_mutex);
            left = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (!left) { by_native.erase(native_identity); by_wrapper.erase(wrapper_identity); }
        }
        if (!left) delete this;
        return left;
    }
    HRESULT get_device(REFIID iid, void** out) noexcept {
        ID3D12DeviceChild* child = nullptr;
        const auto hr = native->QueryInterface(IID_PPV_ARGS(&child));
        if (FAILED(hr)) return hr;
        IUnknown* device = nullptr;
        const auto device_hr = child->GetDevice(iid, reinterpret_cast<void**>(&device));
        child->Release();
        if (FAILED(device_hr)) return device_hr;
        return wrap_object(device, iid, out);
    }
    template<class F> HRESULT create_wrapped(F&& call, REFIID iid, void** out) noexcept {
        if (!out) return call(nullptr);
        *out = nullptr;
        IUnknown* created = nullptr;
        HRESULT hr = call(reinterpret_cast<void**>(&created));
        if (FAILED(hr)) return hr;
        const auto wrapped = wrap_object(created, iid, out);
        return FAILED(wrapped) ? wrapped : hr;
    }
    IUnknown* native;
    ObjectId object;
    IUnknown* native_identity;
    IUnknown* wrapper_identity{};
    std::atomic<ULONG> refs{1};
    std::mutex private_mutex;
    std::vector<GUID> private_interfaces;
};

#include "generated/device_proxy.inc"
#include "generated/device5_proxy.inc"
#include "generated/commandqueue_proxy.inc"
#include "generated/commandallocator_proxy.inc"
#include "generated/graphicscommandlist_proxy.inc"
#include "generated/graphicscommandlist5_proxy.inc"
#include "generated/graphicscommandlist6_proxy.inc"
#include "generated/resource_proxy.inc"
#include "generated/heap_proxy.inc"
#include "generated/descriptorheap_proxy.inc"
#include "generated/fence_proxy.inc"
#include "generated/rootsignature_proxy.inc"
#include "generated/pipelinestate_proxy.inc"
#include "generated/commandsignature_proxy.inc"
#include "generated/queryheap_proxy.inc"

template<class Interface, class Proxy> ProxyBase* make_proxy(IUnknown* native, ObjectKind kind) {
    Interface* typed = nullptr;
    if (FAILED(native->QueryInterface(IID_PPV_ARGS(&typed)))) return nullptr;
    native->Release();
    return new Proxy(typed, frontend_runtime().create_object(kind, reinterpret_cast<std::uintptr_t>(identity(typed))));
}
bool native_supports(IUnknown* native, REFIID iid) {
    IUnknown* probe = nullptr;
    auto ok = SUCCEEDED(native->QueryInterface(iid, reinterpret_cast<void**>(&probe)));
    if (probe) probe->Release();
    return ok;
}
ProxyBase* make_device(IUnknown* native, REFIID requested) {
    if (requested == __uuidof(ID3D12Device5))
        return make_proxy<ID3D12Device5, Device5Proxy>(native, ObjectKind::Device);
    if (native_supports(native, __uuidof(ID3D12Device5)))
        return make_proxy<ID3D12Device5, Device5Proxy>(native, ObjectKind::Device);
    return make_proxy<ID3D12Device, DeviceProxy>(native, ObjectKind::Device);
}
ProxyBase* make_command_list(IUnknown* native, REFIID requested) {
    if (requested == __uuidof(ID3D12GraphicsCommandList6))
        return make_proxy<ID3D12GraphicsCommandList6, GraphicsCommandList6Proxy>(native, ObjectKind::CommandList);
    if (requested == __uuidof(ID3D12GraphicsCommandList5)) {
        if (native_supports(native, __uuidof(ID3D12GraphicsCommandList6)))
            return make_proxy<ID3D12GraphicsCommandList6, GraphicsCommandList6Proxy>(native, ObjectKind::CommandList);
        return make_proxy<ID3D12GraphicsCommandList5, GraphicsCommandList5Proxy>(native, ObjectKind::CommandList);
    }
    if (native_supports(native, __uuidof(ID3D12GraphicsCommandList6)))
        if (auto* p = make_proxy<ID3D12GraphicsCommandList6, GraphicsCommandList6Proxy>(native, ObjectKind::CommandList)) return p;
    if (native_supports(native, __uuidof(ID3D12GraphicsCommandList5)))
        if (auto* p = make_proxy<ID3D12GraphicsCommandList5, GraphicsCommandList5Proxy>(native, ObjectKind::CommandList)) return p;
    return make_proxy<ID3D12GraphicsCommandList, GraphicsCommandListProxy>(native, ObjectKind::CommandList);
}
ProxyBase* construct(IUnknown* native, REFIID iid) {
    if (iid == __uuidof(ID3D12Device) || iid == __uuidof(ID3D12Device1) ||
        iid == __uuidof(ID3D12Device2) || iid == __uuidof(ID3D12Device3) ||
        iid == __uuidof(ID3D12Device4) || iid == __uuidof(ID3D12Device5)) return make_device(native, iid);
    if (iid == __uuidof(ID3D12CommandQueue)) return make_proxy<ID3D12CommandQueue, CommandQueueProxy>(native, ObjectKind::Queue);
    if (iid == __uuidof(ID3D12CommandAllocator)) return make_proxy<ID3D12CommandAllocator, CommandAllocatorProxy>(native, ObjectKind::Allocator);
    if (iid == __uuidof(ID3D12GraphicsCommandList) || iid == __uuidof(ID3D12CommandList) ||
        iid == __uuidof(ID3D12GraphicsCommandList1) || iid == __uuidof(ID3D12GraphicsCommandList2) ||
        iid == __uuidof(ID3D12GraphicsCommandList3) || iid == __uuidof(ID3D12GraphicsCommandList4) ||
        iid == __uuidof(ID3D12GraphicsCommandList5) || iid == __uuidof(ID3D12GraphicsCommandList6)) return make_command_list(native, iid);
    if (iid == __uuidof(ID3D12Resource)) return make_proxy<ID3D12Resource, ResourceProxy>(native, ObjectKind::Resource);
    if (iid == __uuidof(ID3D12Heap)) return make_proxy<ID3D12Heap, HeapProxy>(native, ObjectKind::Heap);
    if (iid == __uuidof(ID3D12DescriptorHeap)) return make_proxy<ID3D12DescriptorHeap, DescriptorHeapProxy>(native, ObjectKind::DescriptorHeap);
    if (iid == __uuidof(ID3D12Fence)) return make_proxy<ID3D12Fence, FenceProxy>(native, ObjectKind::Fence);
    if (iid == __uuidof(ID3D12RootSignature)) return make_proxy<ID3D12RootSignature, RootSignatureProxy>(native, ObjectKind::RootSignature);
    if (iid == __uuidof(ID3D12PipelineState)) return make_proxy<ID3D12PipelineState, PipelineStateProxy>(native, ObjectKind::PipelineState);
    if (iid == __uuidof(ID3D12CommandSignature)) return make_proxy<ID3D12CommandSignature, CommandSignatureProxy>(native, ObjectKind::CommandSignature);
    if (iid == __uuidof(ID3D12QueryHeap)) return make_proxy<ID3D12QueryHeap, QueryHeapProxy>(native, ObjectKind::QueryHeap);
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) ||
        iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12Pageable)) {
        auto probe = [&](REFIID wanted) { IUnknown* p = nullptr; auto ok = SUCCEEDED(native->QueryInterface(wanted, reinterpret_cast<void**>(&p))); if (p) p->Release(); return ok; };
        if (probe(__uuidof(ID3D12Device))) return make_device(native, iid);
        if (probe(__uuidof(ID3D12CommandQueue))) return make_proxy<ID3D12CommandQueue, CommandQueueProxy>(native, ObjectKind::Queue);
        if (probe(__uuidof(ID3D12GraphicsCommandList))) return make_command_list(native, iid);
        if (probe(__uuidof(ID3D12CommandAllocator))) return make_proxy<ID3D12CommandAllocator, CommandAllocatorProxy>(native, ObjectKind::Allocator);
        if (probe(__uuidof(ID3D12Resource))) return make_proxy<ID3D12Resource, ResourceProxy>(native, ObjectKind::Resource);
        if (probe(__uuidof(ID3D12DescriptorHeap))) return make_proxy<ID3D12DescriptorHeap, DescriptorHeapProxy>(native, ObjectKind::DescriptorHeap);
        if (probe(__uuidof(ID3D12Heap))) return make_proxy<ID3D12Heap, HeapProxy>(native, ObjectKind::Heap);
        if (probe(__uuidof(ID3D12Fence))) return make_proxy<ID3D12Fence, FenceProxy>(native, ObjectKind::Fence);
        if (probe(__uuidof(ID3D12RootSignature))) return make_proxy<ID3D12RootSignature, RootSignatureProxy>(native, ObjectKind::RootSignature);
        if (probe(__uuidof(ID3D12PipelineState))) return make_proxy<ID3D12PipelineState, PipelineStateProxy>(native, ObjectKind::PipelineState);
        if (probe(__uuidof(ID3D12CommandSignature))) return make_proxy<ID3D12CommandSignature, CommandSignatureProxy>(native, ObjectKind::CommandSignature);
        if (probe(__uuidof(ID3D12QueryHeap))) return make_proxy<ID3D12QueryHeap, QueryHeapProxy>(native, ObjectKind::QueryHeap);
    }
    return nullptr;
}
} // namespace

Runtime& frontend_runtime() noexcept { return runtime(); }
void set_clear_skip_admit(ClearSkipAdmit callback) noexcept { clear_admit.store(callback); }
IUnknown* unwrap_unknown(IUnknown* object) noexcept {
    if (!object) return nullptr;
    std::lock_guard lock(registry_mutex);
    auto it = by_wrapper.find(object);
    return it == by_wrapper.end() ? object : it->second->native;
}
std::uint64_t object_id(IUnknown* object) noexcept {
    if (!object) return 0;
    std::lock_guard lock(registry_mutex);
    auto it = by_wrapper.find(object);
    if (it != by_wrapper.end()) return it->second->object.value;
    auto n = by_native.find(object);
    return n == by_native.end() ? 0 : n->second->object.value;
}
HRESULT wrap_object(IUnknown* native, REFIID iid, void** out) noexcept {
    debug_trace("wrap_object", native, out);
    if (!out) { if (native) native->Release(); return E_POINTER; }
    *out = nullptr;
    if (!native) return E_POINTER;
    auto key = identity(native);
    if (!key) { native->Release(); return E_NOINTERFACE; }
    ProxyBase* proxy{};
    bool fresh = false;
    {
        std::lock_guard lock(registry_mutex);
        auto it = by_native.find(key);
        if (it != by_native.end()) {
            proxy = it->second;
            if (proxy->supports(iid)) proxy->add_ref();
            else proxy = nullptr;
        } else {
            proxy = construct(native, iid);
            if (proxy) {
                fresh = true;
                // D3D12 interfaces use one IUnknown subobject.
                proxy->wrapper_identity = reinterpret_cast<IUnknown*>(proxy->interface_ptr());
                by_native.emplace(key, proxy);
                by_wrapper.emplace(proxy->wrapper_identity, proxy);
            }
        }
    }
    if (!fresh) native->Release();
    if (!proxy) {
        const auto name = "wrap_object unsupported " + iid_name(iid);
        debug_trace(name.c_str(), native);
        frontend_runtime().note_coverage(name);
        return E_NOINTERFACE;
    }
    if (fresh && proxy->supports(__uuidof(ID3D12DescriptorHeap)))
        register_descriptor_heap(proxy->object, static_cast<ID3D12DescriptorHeap*>(proxy->native));
    if (fresh && proxy->supports(__uuidof(ID3D12Resource)))
        register_resource(proxy->object, static_cast<ID3D12Resource*>(proxy->native));
    *out = proxy->interface_ptr();
    return S_OK;
}
DescriptorRef resolve_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept { return resolve_cpu(handle); }
HRESULT create_device(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** out) noexcept {
    init_diagnostics();
    debug_trace("D3D12CreateDevice", adapter, out);
    if (out) *out = nullptr;
    static auto module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return HRESULT_FROM_WIN32(GetLastError());
    using Fn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(module, "D3D12CreateDevice"));
    if (!fn) return E_NOINTERFACE;
    if (!out) return fn(unwrap_unknown(adapter), level, iid, nullptr);
    IUnknown* native = nullptr;
    auto hr = fn(unwrap_unknown(adapter), level, __uuidof(ID3D12Device), reinterpret_cast<void**>(&native));
    if (FAILED(hr)) return hr;
    auto wrapped = wrap_object(native, iid, out);
    if (FAILED(wrapped)) frontend_runtime().note_coverage("D3D12CreateDevice " + iid_name(iid));
    return FAILED(wrapped) ? wrapped : hr;
}
} // namespace arc::arc2

extern "C" __declspec(dllexport) HRESULT WINAPI Arc2D3D12CreateDevice(
    IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** out) noexcept {
    return arc::arc2::create_device(adapter, level, iid, out);
}
