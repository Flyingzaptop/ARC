#include "../scripts/arc2/atl-compat/atlbase.h"
#include <cassert>

namespace {
int destroyed = 0;
struct Fake final : IUnknown {
    ULONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!InlineIsEqualGUID(id, IID_IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs;
        if (!left) { ++destroyed; delete this; }
        return left;
    }
};
}

int main() {
    auto* raw = new Fake;
    CComPtr<IUnknown> first;
    first.Attach(raw);
    assert(raw->refs == 1);
    CComPtr<IUnknown> second{first};
    assert(raw->refs == 2);
    CComPtr<IUnknown> third;
    third = second;
    assert(raw->refs == 3);
    third = third;
    assert(raw->refs == 3);
    CComPtr<IUnknown> moved{std::move(third)};
    assert(!third && raw->refs == 3);
    CComPtr<IUnknown> queried;
    assert(SUCCEEDED(first.QueryInterface(&queried)) && raw->refs == 4);
    queried.Release();
    assert(raw->refs == 3);
    IUnknown* detached = moved.Detach();
    assert(!moved && raw->refs == 3);
    detached->Release();
    second.Release();
    first.Release();
    assert(destroyed == 1);
}
