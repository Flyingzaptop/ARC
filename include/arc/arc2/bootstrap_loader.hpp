#pragma once
// Include AFTER native D3D12/DXGI declarations, only in graphics bootstrap TUs.
// ARC2_MODE unset: native path. Otherwise load the same generic frontend DLL.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
namespace arc2_bootstrap {
template<class T> inline T native_proc(const wchar_t* library,const char* name){auto module=GetModuleHandleW(library);if(!module)module=LoadLibraryExW(library,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);return module?reinterpret_cast<T>(GetProcAddress(module,name)):nullptr;}
struct Loader {
 HMODULE module{};bool requested{};
 Loader(){wchar_t mode[32]{};requested=GetEnvironmentVariableW(L"ARC2_MODE",mode,32)>0;if(!requested)return;
 wchar_t path[32768]{};DWORD n=GetEnvironmentVariableW(L"ARC2_DLL",path,32768);
 module=LoadLibraryExW(n&&n<32768?path:L"arc2-frontend.dll",nullptr,LOAD_LIBRARY_SEARCH_DEFAULT_DIRS|(n&&n<32768?LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR:0));
 }
 ~Loader() = default; // Never call into another CRT during module/process teardown.

};
inline Loader& loader(){static Loader value;return value;}
inline HRESULT flush(){auto& l=loader();if(!l.module)return S_FALSE;wchar_t path[32768]{};auto n=GetEnvironmentVariableW(L"ARC2_OUTPUT",path,32768);if(!n||n>=32768)return E_INVALIDARG;auto dump=reinterpret_cast<HRESULT(WINAPI*)(const wchar_t*)>(GetProcAddress(l.module,"Arc2Dump"));return dump?dump(path):E_NOINTERFACE;}
inline HRESULT WINAPI device(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** out){
 auto& l=loader();if(!l.requested){auto fn=native_proc<decltype(&::D3D12CreateDevice)>(L"d3d12.dll","D3D12CreateDevice");return fn?fn(adapter,level,iid,out):E_NOINTERFACE;}
 auto fn=l.module?reinterpret_cast<decltype(&D3D12CreateDevice)>(GetProcAddress(l.module,"Arc2D3D12CreateDevice")):nullptr;
 if(!fn){if(out)*out=nullptr;return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);}return fn(adapter,level,iid,out);
}
inline HRESULT WINAPI factory2(UINT flags,REFIID iid,void** out){auto& l=loader();if(!l.requested){auto fn=native_proc<decltype(&::CreateDXGIFactory2)>(L"dxgi.dll","CreateDXGIFactory2");return fn?fn(flags,iid,out):E_NOINTERFACE;}auto fn=l.module?reinterpret_cast<decltype(&CreateDXGIFactory2)>(GetProcAddress(l.module,"Arc2CreateDXGIFactory2")):nullptr;if(!fn){if(out)*out=nullptr;return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);}return fn(flags,iid,out);}
inline HRESULT WINAPI factory1(REFIID iid,void** out){auto& l=loader();if(!l.requested){auto fn=native_proc<decltype(&::CreateDXGIFactory1)>(L"dxgi.dll","CreateDXGIFactory1");return fn?fn(iid,out):E_NOINTERFACE;}auto fn=l.module?reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(l.module,"Arc2CreateDXGIFactory1")):nullptr;if(!fn){if(out)*out=nullptr;return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);}return fn(iid,out);}
inline HRESULT WINAPI factory(REFIID iid,void** out){auto& l=loader();if(!l.requested){auto fn=native_proc<decltype(&::CreateDXGIFactory)>(L"dxgi.dll","CreateDXGIFactory");return fn?fn(iid,out):E_NOINTERFACE;}auto fn=l.module?reinterpret_cast<decltype(&CreateDXGIFactory)>(GetProcAddress(l.module,"Arc2CreateDXGIFactory")):nullptr;if(!fn){if(out)*out=nullptr;return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);}return fn(iid,out);}
}
