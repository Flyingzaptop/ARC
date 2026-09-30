// Mechanical forwarding from Windows SDK 10.0.26100.0.
HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override { ARC2_CPU_SCOPE(DXGI_SetPrivateData); return ARC2_APP_CALL(native_, SetPrivateData, Name, DataSize, pData); }
HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override { ARC2_CPU_SCOPE(DXGI_GetPrivateData); return ARC2_APP_CALL(native_, GetPrivateData, Name, pDataSize, pData); }
HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) override { ARC2_CPU_SCOPE(DXGI_SetFullscreenState); return ARC2_APP_CALL(native_, SetFullscreenState, Fullscreen, pTarget); }
HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) override { ARC2_CPU_SCOPE(DXGI_GetFullscreenState); return ARC2_APP_CALL(native_, GetFullscreenState, pFullscreen, ppTarget); }
HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) override { ARC2_CPU_SCOPE(DXGI_GetDesc); return ARC2_APP_CALL(native_, GetDesc, pDesc); }
HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) override { ARC2_CPU_SCOPE(DXGI_ResizeTarget); return ARC2_APP_CALL(native_, ResizeTarget, pNewTargetParameters); }
HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **ppOutput) override { ARC2_CPU_SCOPE(DXGI_GetContainingOutput); return ARC2_APP_CALL(native_, GetContainingOutput, ppOutput); }
HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) override { ARC2_CPU_SCOPE(DXGI_GetFrameStatistics); return ARC2_APP_CALL(native_, GetFrameStatistics, pStats); }
HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) override { ARC2_CPU_SCOPE(DXGI_GetLastPresentCount); return ARC2_APP_CALL(native_, GetLastPresentCount, pLastPresentCount); }
HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) override { ARC2_CPU_SCOPE(DXGI_GetDesc1); return ARC2_APP_CALL(native_, GetDesc1, pDesc); }
HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) override { ARC2_CPU_SCOPE(DXGI_GetFullscreenDesc); return ARC2_APP_CALL(native_, GetFullscreenDesc, pDesc); }
HRESULT STDMETHODCALLTYPE GetHwnd(HWND *pHwnd) override { ARC2_CPU_SCOPE(DXGI_GetHwnd); return ARC2_APP_CALL(native_, GetHwnd, pHwnd); }
HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void **ppUnk) override { ARC2_CPU_SCOPE(DXGI_GetCoreWindow); return ARC2_APP_CALL(native_, GetCoreWindow, refiid, ppUnk); }
BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void) override { ARC2_CPU_SCOPE(DXGI_IsTemporaryMonoSupported); return ARC2_APP_CALL(native_, IsTemporaryMonoSupported); }
HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) override { ARC2_CPU_SCOPE(DXGI_GetRestrictToOutput); return ARC2_APP_CALL(native_, GetRestrictToOutput, ppRestrictToOutput); }
HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *pColor) override { ARC2_CPU_SCOPE(DXGI_SetBackgroundColor); return ARC2_APP_CALL(native_, SetBackgroundColor, pColor); }
HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *pColor) override { ARC2_CPU_SCOPE(DXGI_GetBackgroundColor); return ARC2_APP_CALL(native_, GetBackgroundColor, pColor); }
HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override { ARC2_CPU_SCOPE(DXGI_SetRotation); return ARC2_APP_CALL(native_, SetRotation, Rotation); }
HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *pRotation) override { ARC2_CPU_SCOPE(DXGI_GetRotation); return ARC2_APP_CALL(native_, GetRotation, pRotation); }
HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override { ARC2_CPU_SCOPE(DXGI_SetSourceSize); return ARC2_APP_CALL(native_, SetSourceSize, Width, Height); }
HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *pWidth, UINT *pHeight) override { ARC2_CPU_SCOPE(DXGI_GetSourceSize); return ARC2_APP_CALL(native_, GetSourceSize, pWidth, pHeight); }
HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override { ARC2_CPU_SCOPE(DXGI_SetMaximumFrameLatency); return ARC2_APP_CALL(native_, SetMaximumFrameLatency, MaxLatency); }
HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override { ARC2_CPU_SCOPE(DXGI_GetMaximumFrameLatency); return ARC2_APP_CALL(native_, GetMaximumFrameLatency, pMaxLatency); }
HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void) override { ARC2_CPU_SCOPE(DXGI_GetFrameLatencyWaitableObject); return ARC2_APP_CALL(native_, GetFrameLatencyWaitableObject); }
HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *pMatrix) override { ARC2_CPU_SCOPE(DXGI_SetMatrixTransform); return ARC2_APP_CALL(native_, SetMatrixTransform, pMatrix); }
HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *pMatrix) override { ARC2_CPU_SCOPE(DXGI_GetMatrixTransform); return ARC2_APP_CALL(native_, GetMatrixTransform, pMatrix); }
UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void) override { ARC2_CPU_SCOPE(DXGI_GetCurrentBackBufferIndex); return ARC2_APP_CALL(native_, GetCurrentBackBufferIndex); }
HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) override { ARC2_CPU_SCOPE(DXGI_CheckColorSpaceSupport); return ARC2_APP_CALL(native_, CheckColorSpaceSupport, ColorSpace, pColorSpaceSupport); }
HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override { ARC2_CPU_SCOPE(DXGI_SetColorSpace1); return ARC2_APP_CALL(native_, SetColorSpace1, ColorSpace); }
HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void *pMetaData) override { ARC2_CPU_SCOPE(DXGI_SetHDRMetaData); return ARC2_APP_CALL(native_, SetHDRMetaData, Type, Size, pMetaData); }
