// Mechanical forwarding from Windows SDK 10.0.26100.0.
HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override { return native_->SetPrivateData(Name, DataSize, pData); }
HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override { return native_->GetPrivateData(Name, pDataSize, pData); }
HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) override { return native_->SetFullscreenState(Fullscreen, pTarget); }
HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) override { return native_->GetFullscreenState(pFullscreen, ppTarget); }
HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) override { return native_->GetDesc(pDesc); }
HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) override { return native_->ResizeTarget(pNewTargetParameters); }
HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **ppOutput) override { return native_->GetContainingOutput(ppOutput); }
HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) override { return native_->GetFrameStatistics(pStats); }
HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) override { return native_->GetLastPresentCount(pLastPresentCount); }
HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) override { return native_->GetDesc1(pDesc); }
HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) override { return native_->GetFullscreenDesc(pDesc); }
HRESULT STDMETHODCALLTYPE GetHwnd(HWND *pHwnd) override { return native_->GetHwnd(pHwnd); }
HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void **ppUnk) override { return native_->GetCoreWindow(refiid, ppUnk); }
BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void) override { return native_->IsTemporaryMonoSupported(); }
HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) override { return native_->GetRestrictToOutput(ppRestrictToOutput); }
HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *pColor) override { return native_->SetBackgroundColor(pColor); }
HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *pColor) override { return native_->GetBackgroundColor(pColor); }
HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override { return native_->SetRotation(Rotation); }
HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *pRotation) override { return native_->GetRotation(pRotation); }
HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override { return native_->SetSourceSize(Width, Height); }
HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *pWidth, UINT *pHeight) override { return native_->GetSourceSize(pWidth, pHeight); }
HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override { return native_->SetMaximumFrameLatency(MaxLatency); }
HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override { return native_->GetMaximumFrameLatency(pMaxLatency); }
HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void) override { return native_->GetFrameLatencyWaitableObject(); }
HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *pMatrix) override { return native_->SetMatrixTransform(pMatrix); }
HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *pMatrix) override { return native_->GetMatrixTransform(pMatrix); }
UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void) override { return native_->GetCurrentBackBufferIndex(); }
HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) override { return native_->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport); }
HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override { return native_->SetColorSpace1(ColorSpace); }
HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void *pMetaData) override { return native_->SetHDRMetaData(Type, Size, pMetaData); }
