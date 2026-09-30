// Generated from d3d12.h; changes belong in generate_frontend.py or overrides.
class RootSignatureProxy final : public ID3D12RootSignature, public ProxyBase {
public:
    RootSignatureProxy(ID3D12RootSignature* native, ObjectId id) : ProxyBase(native, id), native_(native) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { return query(iid, out); }
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    bool supports(REFIID iid) const noexcept override { return iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12RootSignature); }
    void* interface_ptr() noexcept override { return static_cast<ID3D12RootSignature*>(this); }
    const char* type_name() const noexcept override { return "RootSignature"; }
private:
    ID3D12RootSignature* native_;
public:
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override { debug_trace("RootSignature.GetPrivateData", this, native_, object.value); return get_private_data(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override { debug_trace("RootSignature.SetPrivateData", this, native_, object.value); return set_private_bytes(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override { debug_trace("RootSignature.SetPrivateDataInterface", this, native_, object.value); return set_private_interface(guid, pData); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override { debug_trace("RootSignature.SetName", this, native_, object.value); return native_->SetName(native_arg(Name)); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppvDevice) override { debug_trace("RootSignature.GetDevice", this, native_, object.value); return get_device(riid, ppvDevice); }
};
