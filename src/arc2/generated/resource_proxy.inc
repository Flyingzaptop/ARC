// Generated from d3d12.h; changes belong in generate_frontend.py or overrides.
class ResourceProxy final : public ID3D12Resource, public ProxyBase {
public:
    ResourceProxy(ID3D12Resource* native, ObjectId id) : ProxyBase(native, id), native_(native) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { ARC2_CPU_SCOPE(D3D12_QueryInterface); return query(iid, out); }
    ULONG STDMETHODCALLTYPE AddRef() override { ARC2_CPU_SCOPE(D3D12_AddRef); return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { ARC2_CPU_SCOPE(D3D12_Release); return release(); }
    bool supports(REFIID iid) const noexcept override { return iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12Pageable) || iid == __uuidof(ID3D12Resource); }
    void* interface_ptr() noexcept override { return static_cast<ID3D12Resource*>(this); }
    const char* type_name() const noexcept override { return "Resource"; }
private:
    ID3D12Resource* native_;
public:
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override { ARC2_CPU_SCOPE(D3D12_GetPrivateData); debug_trace("Resource.GetPrivateData", this, native_, object.value); return get_private_data(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override { ARC2_CPU_SCOPE(D3D12_SetPrivateData); debug_trace("Resource.SetPrivateData", this, native_, object.value); return set_private_bytes(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override { ARC2_CPU_SCOPE(D3D12_SetPrivateDataInterface); debug_trace("Resource.SetPrivateDataInterface", this, native_, object.value); return set_private_interface(guid, pData); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override { ARC2_CPU_SCOPE(D3D12_SetName); debug_trace("Resource.SetName", this, native_, object.value); return ARC2_APP_CALL(native_, SetName, native_arg(Name)); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppvDevice) override { ARC2_CPU_SCOPE(D3D12_GetDevice); debug_trace("Resource.GetDevice", this, native_, object.value); return get_device(riid, ppvDevice); }
    HRESULT STDMETHODCALLTYPE Map(UINT Subresource, const D3D12_RANGE *pReadRange, void **ppData) override { ARC2_CPU_SCOPE(D3D12_Map); debug_trace("Resource.Map", this, native_, object.value); return ARC2_APP_CALL(native_, Map, native_arg(Subresource), native_arg(pReadRange), native_arg(ppData)); }
    void STDMETHODCALLTYPE Unmap(UINT Subresource, const D3D12_RANGE *pWrittenRange) override { ARC2_CPU_SCOPE(D3D12_Unmap); debug_trace("Resource.Unmap", this, native_, object.value); ARC2_APP_CALL(native_, Unmap, native_arg(Subresource), native_arg(pWrittenRange)); }
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc(void) override { ARC2_CPU_SCOPE(D3D12_GetDesc); debug_trace("Resource.GetDesc", this, native_, object.value); return ARC2_APP_CALL(native_, GetDesc); }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress(void) override { ARC2_CPU_SCOPE(D3D12_GetGPUVirtualAddress); debug_trace("Resource.GetGPUVirtualAddress", this, native_, object.value); return ARC2_APP_CALL(native_, GetGPUVirtualAddress); }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT DstSubresource, const D3D12_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override { ARC2_CPU_SCOPE(D3D12_WriteToSubresource); debug_trace("Resource.WriteToSubresource", this, native_, object.value); return ARC2_APP_CALL(native_, WriteToSubresource, native_arg(DstSubresource), native_arg(pDstBox), native_arg(pSrcData), native_arg(SrcRowPitch), native_arg(SrcDepthPitch)); }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void *pDstData, UINT DstRowPitch, UINT DstDepthPitch, UINT SrcSubresource, const D3D12_BOX *pSrcBox) override { ARC2_CPU_SCOPE(D3D12_ReadFromSubresource); debug_trace("Resource.ReadFromSubresource", this, native_, object.value); return ARC2_APP_CALL(native_, ReadFromSubresource, native_arg(pDstData), native_arg(DstRowPitch), native_arg(DstDepthPitch), native_arg(SrcSubresource), native_arg(pSrcBox)); }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS *pHeapFlags) override { ARC2_CPU_SCOPE(D3D12_GetHeapProperties); debug_trace("Resource.GetHeapProperties", this, native_, object.value); return ARC2_APP_CALL(native_, GetHeapProperties, native_arg(pHeapProperties), native_arg(pHeapFlags)); }
};
