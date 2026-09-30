// Generated from d3d12.h; changes belong in generate_frontend.py or overrides.
class HeapProxy final : public ID3D12Heap, public ProxyBase {
public:
    HeapProxy(ID3D12Heap* native, ObjectId id) : ProxyBase(native, id), native_(native) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { ARC2_CPU_SCOPE(D3D12_QueryInterface); return query(iid, out); }
    ULONG STDMETHODCALLTYPE AddRef() override { ARC2_CPU_SCOPE(D3D12_AddRef); return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { ARC2_CPU_SCOPE(D3D12_Release); return release(); }
    bool supports(REFIID iid) const noexcept override { return iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12Pageable) || iid == __uuidof(ID3D12Heap); }
    void* interface_ptr() noexcept override { return static_cast<ID3D12Heap*>(this); }
    const char* type_name() const noexcept override { return "Heap"; }
private:
    ID3D12Heap* native_;
public:
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override { ARC2_CPU_SCOPE(D3D12_GetPrivateData); debug_trace("Heap.GetPrivateData", this, native_, object.value); return get_private_data(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override { ARC2_CPU_SCOPE(D3D12_SetPrivateData); debug_trace("Heap.SetPrivateData", this, native_, object.value); return set_private_bytes(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override { ARC2_CPU_SCOPE(D3D12_SetPrivateDataInterface); debug_trace("Heap.SetPrivateDataInterface", this, native_, object.value); return set_private_interface(guid, pData); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override { ARC2_CPU_SCOPE(D3D12_SetName); debug_trace("Heap.SetName", this, native_, object.value); return ARC2_APP_CALL(native_, SetName, native_arg(Name)); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppvDevice) override { ARC2_CPU_SCOPE(D3D12_GetDevice); debug_trace("Heap.GetDevice", this, native_, object.value); return get_device(riid, ppvDevice); }
    D3D12_HEAP_DESC STDMETHODCALLTYPE GetDesc(void) override { ARC2_CPU_SCOPE(D3D12_GetDesc); debug_trace("Heap.GetDesc", this, native_, object.value); return ARC2_APP_CALL(native_, GetDesc); }
};
