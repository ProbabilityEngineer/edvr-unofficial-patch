#pragma once

#include "texture_lifetime.h"
#include <d3d11.h>
#include <atomic>
#include <new>

namespace edvr::texture_lifetime {

inline const GUID kTagGuid =
    {0x24de6e6b, 0x5b97, 0x4b43, {0x92, 0xd9, 0xe2, 0x64, 0x90, 0x8e, 0x37, 0xa1}};

inline Key keyFor(const D3D11_TEXTURE2D_DESC& d, bool fromEdvr) {
    return {fromEdvr, {d.Width, d.Height, UINT(d.Format), d.BindFlags,
        d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.SampleDesc.Quality,
        UINT(d.Usage), d.CPUAccessFlags, d.MiscFlags}};
}

// Only the resource owns a reference to this tag, never the reverse.
// The production ledger and module must outlive all tags.
class Tag final : public IUnknown {
public:
    Tag(Ledger& ledger, unsigned slot, uint64_t bytes)
        : ledger_(ledger), slot_(slot), bytes_(bytes) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, __uuidof(IUnknown))) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) {
            if (armed_) ledger_.destroyed(slot_, bytes_);
            delete this;
        }
        return left;
    }
    void arm() {
        ledger_.attached(slot_, bytes_);
        armed_ = true;
    }

private:
    std::atomic<ULONG> refs_{1};
    Ledger& ledger_;
    unsigned slot_;
    uint64_t bytes_;
    bool armed_ = false;
};

// Called once per successful CreateTexture2D result, while the caller owns
// its initial resource reference. No locks span a driver/COM call.
template<class Resource>
void attach(Ledger& ledger, Resource* resource, const Key& key, uint64_t bytes) {
    const unsigned slot = ledger.reserve(key);
    if (slot == kInvalid) return;
    auto* tag = new (std::nothrow) Tag(ledger, slot, bytes);
    if (!tag) {
        ledger.failed();
        return;
    }
    const HRESULT hr = resource->SetPrivateDataInterface(kTagGuid, tag);
    if (SUCCEEDED(hr)) tag->arm();
    else ledger.failed();
    tag->Release();
}

} // namespace edvr::texture_lifetime
