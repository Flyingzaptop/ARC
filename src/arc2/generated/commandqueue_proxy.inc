// Generated from d3d12.h; changes belong in generate_frontend.py or overrides.
class CommandQueueProxy final : public ID3D12CommandQueue, public ProxyBase {
public:
    CommandQueueProxy(ID3D12CommandQueue* native, ObjectId id) : ProxyBase(native, id), native_(native) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { return query(iid, out); }
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    bool supports(REFIID iid) const noexcept override { return iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12Pageable) || iid == __uuidof(ID3D12CommandQueue); }
    void* interface_ptr() noexcept override { return static_cast<ID3D12CommandQueue*>(this); }
    const char* type_name() const noexcept override { return "CommandQueue"; }
private:
    ID3D12CommandQueue* native_;
public:
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override { debug_trace("CommandQueue.GetPrivateData", this, native_, object.value); return get_private_data(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override { debug_trace("CommandQueue.SetPrivateData", this, native_, object.value); return set_private_bytes(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override { debug_trace("CommandQueue.SetPrivateDataInterface", this, native_, object.value); return set_private_interface(guid, pData); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override { debug_trace("CommandQueue.SetName", this, native_, object.value); return native_->SetName(native_arg(Name)); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppvDevice) override { debug_trace("CommandQueue.GetDevice", this, native_, object.value); return get_device(riid, ppvDevice); }
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *pResource, UINT NumResourceRegions, const D3D12_TILED_RESOURCE_COORDINATE *pResourceRegionStartCoordinates, const D3D12_TILE_REGION_SIZE *pResourceRegionSizes, ID3D12Heap *pHeap, UINT NumRanges, const D3D12_TILE_RANGE_FLAGS *pRangeFlags, const UINT *pHeapRangeStartOffsets, const UINT *pRangeTileCounts, D3D12_TILE_MAPPING_FLAGS Flags) override { debug_trace("CommandQueue.UpdateTileMappings", this, native_, object.value); native_->UpdateTileMappings(native_arg(pResource), native_arg(NumResourceRegions), native_arg(pResourceRegionStartCoordinates), native_arg(pResourceRegionSizes), native_arg(pHeap), native_arg(NumRanges), native_arg(pRangeFlags), native_arg(pHeapRangeStartOffsets), native_arg(pRangeTileCounts), native_arg(Flags)); }
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *pDstResource, const D3D12_TILED_RESOURCE_COORDINATE *pDstRegionStartCoordinate, ID3D12Resource *pSrcResource, const D3D12_TILED_RESOURCE_COORDINATE *pSrcRegionStartCoordinate, const D3D12_TILE_REGION_SIZE *pRegionSize, D3D12_TILE_MAPPING_FLAGS Flags) override { debug_trace("CommandQueue.CopyTileMappings", this, native_, object.value); native_->CopyTileMappings(native_arg(pDstResource), native_arg(pDstRegionStartCoordinate), native_arg(pSrcResource), native_arg(pSrcRegionStartCoordinate), native_arg(pRegionSize), native_arg(Flags)); }
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList *const *ppCommandLists) override { debug_trace("CommandQueue.ExecuteCommandLists", this, native_, object.value); execute_lists(native_, object, NumCommandLists, ppCommandLists); }
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *pData, UINT Size) override { debug_trace("CommandQueue.SetMarker", this, native_, object.value); native_->SetMarker(native_arg(Metadata), native_arg(pData), native_arg(Size)); }
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *pData, UINT Size) override { debug_trace("CommandQueue.BeginEvent", this, native_, object.value); native_->BeginEvent(native_arg(Metadata), native_arg(pData), native_arg(Size)); }
    void STDMETHODCALLTYPE EndEvent(void) override { debug_trace("CommandQueue.EndEvent", this, native_, object.value); native_->EndEvent(); }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *pFence, UINT64 Value) override { debug_trace("CommandQueue.Signal", this, native_, object.value); auto hr = native_->Signal(native_arg(pFence), Value); if (SUCCEEDED(hr)) frontend_runtime().signal(object, oid(pFence), Value); return hr; }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *pFence, UINT64 Value) override { debug_trace("CommandQueue.Wait", this, native_, object.value); auto hr = native_->Wait(native_arg(pFence), Value); if (SUCCEEDED(hr)) frontend_runtime().wait(object, oid(pFence), Value); return hr; }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *pFrequency) override { debug_trace("CommandQueue.GetTimestampFrequency", this, native_, object.value); return native_->GetTimestampFrequency(native_arg(pFrequency)); }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *pGpuTimestamp, UINT64 *pCpuTimestamp) override { debug_trace("CommandQueue.GetClockCalibration", this, native_, object.value); return native_->GetClockCalibration(native_arg(pGpuTimestamp), native_arg(pCpuTimestamp)); }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc(void) override { debug_trace("CommandQueue.GetDesc", this, native_, object.value); return native_->GetDesc(); }
};
