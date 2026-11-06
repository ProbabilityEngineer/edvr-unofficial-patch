#pragma once

#include "../../src/d3d11/texture_lifetime.h"
#include <thread>
#include <memory>
#ifdef _WIN32
#include "../../src/d3d11/texture_lifetime_d3d11.h"
#endif

namespace texture_lifetime_tests {
using namespace edvr::texture_lifetime;

template<class Check> void run(Check check) {
    Ledger ledger;
    Key key{};
    key.desc[0] = 64;
    unsigned a = ledger.reserve(key);
    ledger.attached(a, 256);
    unsigned b = ledger.reserve(key);
    ledger.attached(b, 512);
    check(a == b, "texture lifetime reuses identical descriptor slots");
    auto s = ledger.snapshot();
    check(s.observed == 2 && s.pending == 0 && s.totals[0].live == 2 &&
          s.totals[0].bytes == 768, "texture lifetime counts attached objects and bytes");
    ledger.destroyed(a, 256);
    ledger.destroyed(b, 512);
    s = ledger.snapshot();
    check(s.totals[0].live == 0 && s.totals[0].bytes == 0 &&
          s.totals[0].destroyed == 2 && s.totals[0].peak == 2 &&
          s.totals[0].peakBytes == 768, "texture lifetime releases current but preserves peak counts");
    key.edvr = true;
    const auto owned = ledger.reserve(key);
    ledger.attached(owned, 128);
    check(owned != a && ledger.snapshot().totals[1].live == 1,
          "texture lifetime separates EDVR caller image from external callers");
    ledger.destroyed(owned, 128);
    ledger.reserve(key);
    ledger.failed();
    s = ledger.snapshot();
    check(s.failed == 1 && s.pending == 0 && s.totals[1].live == 0,
          "texture lifetime attachment failure is visible without phantom live objects");

    Ledger overflow;
    for (unsigned i = 0; i < kDescriptors + 6; ++i) {
        key.desc[0] = i;
        key.edvr = bool(i & 1);
        const auto slot = overflow.reserve(key);
        overflow.attached(slot, 8);
    }
    s = overflow.snapshot();
    check(s.used == kDescriptors &&
          s.groups[kDescriptors].counts.live == 3 &&
          s.groups[kDescriptors + 1].counts.live == 3 &&
          s.totals[0].live + s.totals[1].live == kDescriptors + 6,
          "texture lifetime descriptor overflow remains bounded and accounted per origin");

    Ledger capped;
    key = {};
    for (uint64_t i = 0; i < kMaxTags; ++i)
        capped.attached(capped.reserve(key), 1);
    check(capped.reserve(key) == kInvalid && capped.snapshot().capped == 1,
          "texture lifetime caps active tracking overhead and reports skipped objects");
    capped.destroyed(0, 1);
    const auto reserved = capped.reserve(key);
    check(reserved != kInvalid && capped.reserve(key) == kInvalid,
          "texture lifetime cap recovers after destruction and includes pending attachments");
    capped.failed();

    Snapshot ranking{};
    std::array<uint64_t, kGroups> previous{};
    previous[0] = 1000; // a completely freed group must still appear
    ranking.groups[1].counts.bytes = 500;
    const auto rows = reportGroups(ranking, previous);
    check(rows[0] == 0 && rows[1] == 1 && rows[2] == kInvalid && rows[4] == kInvalid,
          "texture lifetime bounded report includes frees and avoids duplicate groups");

    Ledger threaded;
    std::array<std::thread, 4> threads;
    for (auto& t : threads) t = std::thread([&] {
        for (unsigned i = 0; i < 500; ++i) {
            const auto slot = threaded.reserve(Key{});
            threaded.attached(slot, 16);
            threaded.destroyed(slot, 16);
        }
    });
    for (auto& t : threads) t.join();
    s = threaded.snapshot();
    check(s.totals[0].created == 2000 && s.totals[0].destroyed == 2000 &&
          s.totals[0].live == 0 && s.totals[0].bytes == 0 && s.pending == 0,
          "texture lifetime balances concurrent creator and destruction threads");
}

#ifdef _WIN32
// A COM private-data owner, not a D3D implementation. Confirms the actual
// production tag's reference protocol, including failure and late releases.
struct PrivateDataOwner {
    IUnknown* tag = nullptr;
    HRESULT result = S_OK;
    HRESULT SetPrivateDataInterface(REFGUID guid, const IUnknown* value) {
        if (!IsEqualGUID(guid, kTagGuid)) return E_INVALIDARG;
        if (FAILED(result)) return result;
        if (value) const_cast<IUnknown*>(value)->AddRef();
        if (tag) tag->Release();
        tag = const_cast<IUnknown*>(value);
        return result;
    }
    ~PrivateDataOwner() { if (tag) tag->Release(); }
};

template<class Check> void runCom(Check check) {
    Ledger ledger;
    PrivateDataOwner owner;
    owner.result = E_OUTOFMEMORY;
    attach(ledger, &owner, Key{}, 64);
    check(ledger.snapshot().failed == 1 && ledger.snapshot().totals[0].live == 0,
          "texture lifetime production tag handles SetPrivateDataInterface failure");
    owner.result = S_OK;
    attach(ledger, &owner, Key{}, 64);
    IUnknown* held = nullptr;
    const HRESULT hr = owner.tag->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&held));
    check(SUCCEEDED(hr) && held == owner.tag,
          "texture lifetime tag supports IUnknown with an owning reference");
    void* unknown = reinterpret_cast<void*>(1);
    check(owner.tag->QueryInterface(__uuidof(ID3D11Resource), &unknown) == E_NOINTERFACE &&
          unknown == nullptr && owner.tag->QueryInterface(__uuidof(IUnknown), nullptr) == E_POINTER,
          "texture lifetime tag rejects unsupported interfaces and null outputs");
    owner.SetPrivateDataInterface(kTagGuid, nullptr);
    check(ledger.snapshot().totals[0].live == 1,
          "texture lifetime does not report destruction before the final tag reference");
    if (held) held->Release();
    check(ledger.snapshot().totals[0].destroyed == 1 && ledger.snapshot().totals[0].live == 0,
          "texture lifetime reports final tag release without a resource reference cycle");
}

template<class Check> void runDevice(Check check, ID3D11Device* device) {
    auto storage = std::make_unique<Ledger>();
    auto& ledger = *storage;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 16;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
    check(SUCCEEDED(hr) && texture, "texture lifetime real D3D texture creates");
    if (!texture) return;
    attach(ledger, texture, keyFor(desc, false), 1024);
    check(ledger.snapshot().totals[0].live == 1 && ledger.snapshot().failed == 0,
          "texture lifetime real D3D resource accepts the production tag");
    ID3D11ShaderResourceView* view = nullptr;
    hr = device->CreateShaderResourceView(texture, nullptr, &view);
    check(SUCCEEDED(hr) && view, "texture lifetime real D3D view creates");
    texture->Release();
    if (view) {
        check(ledger.snapshot().totals[0].live == 1,
              "texture lifetime real D3D view keeps its tagged resource alive");
        view->Release();
    }
    check(ledger.snapshot().totals[0].live == 0 &&
          ledger.snapshot().totals[0].destroyed == 1,
          "texture lifetime real D3D final release destroys the tag");
    // A runtime that defers destruction must not leave a tag pointing at
    // dead test storage after reporting the failed immediate-release check.
    if (ledger.snapshot().totals[0].live) (void)storage.release();
}
#endif
} // namespace texture_lifetime_tests
