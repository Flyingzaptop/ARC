// Mechanical forwarding from Windows SDK 10.0.26100.0.
HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override { return native_->SetPrivateData(Name, DataSize, pData); }
HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override { return native_->GetPrivateData(Name, pDataSize, pData); }
HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override { return native_->GetParent(riid, ppParent); }
HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter **ppAdapter) override { return native_->EnumAdapters(Adapter, ppAdapter); }
HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override { return native_->MakeWindowAssociation(WindowHandle, Flags); }
HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) override { return native_->GetWindowAssociation(pWindowHandle); }
HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) override { return native_->CreateSoftwareAdapter(Module, ppAdapter); }
HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1 **ppAdapter) override { return native_->EnumAdapters1(Adapter, ppAdapter); }
BOOL STDMETHODCALLTYPE IsCurrent(void) override { return native_->IsCurrent(); }
BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled(void) override { return native_->IsWindowedStereoEnabled(); }
HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID *pLuid) override { return native_->GetSharedResourceAdapterLuid(hResource, pLuid); }
HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override { return native_->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie); }
HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override { return native_->RegisterStereoStatusEvent(hEvent, pdwCookie); }
void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override { return native_->UnregisterStereoStatus(dwCookie); }
HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override { return native_->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie); }
HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override { return native_->RegisterOcclusionStatusEvent(hEvent, pdwCookie); }
void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override { return native_->UnregisterOcclusionStatus(dwCookie); }
UINT STDMETHODCALLTYPE GetCreationFlags(void) override { return native_->GetCreationFlags(); }
HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void **ppvAdapter) override { return native_->EnumAdapterByLuid(AdapterLuid, riid, ppvAdapter); }
HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void **ppvAdapter) override { return native_->EnumWarpAdapter(riid, ppvAdapter); }
HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) override { return native_->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize); }
HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference, REFIID riid, void **ppvAdapter) override { return native_->EnumAdapterByGpuPreference(Adapter, GpuPreference, riid, ppvAdapter); }
HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD *pdwCookie) override { return native_->RegisterAdaptersChangedEvent(hEvent, pdwCookie); }
HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD dwCookie) override { return native_->UnregisterAdaptersChangedEvent(dwCookie); }
