#pragma once
// Minimal ATL CComPtr compatibility for source-only Diligent DX12 test builds.
// This file lives on a testbed-only include path when VS ATL is unavailable.
#include <Unknwn.h>
#include <objbase.h>
#include <cassert>
#include <memory>
#include <type_traits>
#include <utility>

template <class T>
class CComPtr {
public:
    T* p = nullptr;
    CComPtr() noexcept = default;
    CComPtr(std::nullptr_t) noexcept {}
    CComPtr(T* value) noexcept : p(value) { if (p) p->AddRef(); }
    CComPtr(const CComPtr& other) noexcept : CComPtr(other.p) {}
    template <class U, class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    CComPtr(const CComPtr<U>& other) noexcept : CComPtr(other.p) {}
    CComPtr(CComPtr&& other) noexcept : p(other.Detach()) {}
    ~CComPtr() { Release(); }

    CComPtr& operator=(const CComPtr& other) noexcept {
        if (this != std::addressof(other)) Assign(other.p);
        return *this;
    }
    CComPtr& operator=(CComPtr&& other) noexcept {
        if (this != std::addressof(other)) { Release(); p = other.Detach(); }
        return *this;
    }
    CComPtr& operator=(T* value) noexcept { Assign(value); return *this; }
    CComPtr& operator=(std::nullptr_t) noexcept { Release(); return *this; }
    operator T*() const noexcept { return p; }
    explicit operator bool() const noexcept { return p != nullptr; }
    T* operator->() const noexcept { assert(p); return p; }
    T** operator&() noexcept { assert(!p); return &p; }
    bool operator!() const noexcept { return p == nullptr; }
    void Attach(T* value) noexcept { Release(); p = value; }
    T* Detach() noexcept { T* value = p; p = nullptr; return value; }
    void Release() noexcept { T* value = p; p = nullptr; if (value) value->Release(); }
    template <class U>
    HRESULT QueryInterface(U** out) const noexcept {
        if (!out) return E_POINTER;
        *out = nullptr;
        return p ? p->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out)) : E_POINTER;
    }
    HRESULT CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer = nullptr,
                             DWORD context = CLSCTX_ALL) noexcept {
        Release();
        return ::CoCreateInstance(clsid, outer, context, __uuidof(T),
                                  reinterpret_cast<void**>(&p));
    }
private:
    void Assign(T* value) noexcept { if (value) value->AddRef(); Release(); p = value; }
};

template <class T>
class CComQIPtr : public CComPtr<T> {
public:
    CComQIPtr() noexcept = default;
    CComQIPtr(IUnknown* value) noexcept {
        if (value) value->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&this->p));
    }
    template <class U>
    CComQIPtr(const CComPtr<U>& value) noexcept : CComQIPtr(static_cast<IUnknown*>(value.p)) {}
};
