#pragma once

#include "../../src/d3d11/aa_memory_scope.h"
#include <thread>
#include <memory>
#ifdef _WIN32
#include "../../src/d3d11/aa_memory_diagnostics.h"
#include <wrl/client.h>
#endif

namespace aa_memory_tests {
namespace aa = edvr::aa_memory;

template<class Check> void run(Check check) {
    check(aa::engineRecordsAllowed(false, true) && aa::engineRecordsAllowed(false, false) &&
          aa::engineRecordsAllowed(true, false) && !aa::engineRecordsAllowed(true, true),
          "AA isolation affects only flat record velocity, not VR or the control");
    check(aa::allocationOwner(false) == aa::Owner::None &&
          aa::allocationOwner(true) == aa::Owner::OtherEdvr,
          "AA allocation scope distinguishes unscoped external and EDVR calls");
    bool nested = false, independent = false;
    {
        aa::Scope outer(aa::Owner::Resolver);
        const int result = aa::create(aa::Owner::Engine, [&] {
            nested = aa::allocationOwner(false) == aa::Owner::Engine;
            return 17;
        });
        nested = nested && result == 17 && aa::current == aa::Owner::Resolver;
        std::thread worker([&] {
            independent = aa::current == aa::Owner::None;
            aa::Scope inner(aa::Owner::Foreground);
            independent = independent && aa::allocationOwner(false) == aa::Owner::Foreground;
        });
        worker.join();
        independent = independent && aa::current == aa::Owner::Resolver;
    }
    check(nested && aa::current == aa::Owner::None, "AA nested creation scope restores its parent and result");
    check(independent, "AA allocation scopes do not spill into another thread");
}

#ifdef _WIN32
template<class Check> void runDevice(Check check, ID3D11Device* device) {
    using Microsoft::WRL::ComPtr;
    using namespace edvr::texture_lifetime;
    auto* owners = aa::state();
    check(owners != nullptr, "AA owner ledgers have process lifetime and a pinned module");
    if (!owners) return;
    auto storage = std::make_unique<Ledger>();
    auto& original = *storage;
    auto& ownedTextures = owners->ledger[unsigned(aa::Owner::Engine)][0];
    const auto beforeTexture = ownedTextures.snapshot().totals[1];
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 16;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT textureHr = device->CreateTexture2D(&td, nullptr, &texture);
    check(SUCCEEDED(textureHr) && texture, "AA dual-tag texture creates");
    if (texture) {
        attach(original, texture.Get(), keyFor(td, true), 1024);
        {
            aa::Scope scope(aa::Owner::Engine);
            aa::observe(texture.Get(), false, true, 1024);
        }
        check(original.snapshot().totals[1].live == 1 &&
              ownedTextures.snapshot().totals[1].live == beforeTexture.live + 1,
              "AA owner tag coexists with the original texture lifetime tag");
        texture.Reset();
        check(original.snapshot().totals[1].live == 0 &&
              ownedTextures.snapshot().totals[1].live == beforeTexture.live &&
              ownedTextures.snapshot().totals[1].destroyed == beforeTexture.destroyed + 1,
              "AA dual tags both observe final release without retaining the texture");
    }
    if (original.snapshot().totals[1].live) (void)storage.release();

    auto& buffers = owners->ledger[unsigned(aa::Owner::Foreground)][1];
    const auto beforeBuffer = buffers.snapshot().totals[1];
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 4096; // view uses only 16 bytes; ledger must count full capacity
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 16;
    ComPtr<ID3D11Buffer> buffer;
    const HRESULT bufferHr = device->CreateBuffer(&bd, nullptr, &buffer);
    check(SUCCEEDED(bufferHr) && buffer, "AA capacity-tagged buffer creates");
    if (buffer) {
        {
            aa::Scope scope(aa::Owner::Foreground);
            aa::observe(buffer.Get(), true, false, bd.ByteWidth);
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        vd.Buffer.NumElements = 1;
        ComPtr<ID3D11ShaderResourceView> view;
        const HRESULT viewHr = device->CreateShaderResourceView(buffer.Get(), &vd, &view);
        check(SUCCEEDED(viewHr) && view, "AA buffer view creates");
        buffer.Reset();
        check(view && buffers.snapshot().totals[1].bytes == beforeBuffer.bytes + 4096,
              "AA held view keeps full allocated buffer capacity counted");
        view.Reset();
        check(buffers.snapshot().totals[1].live == beforeBuffer.live &&
              buffers.snapshot().totals[1].bytes == beforeBuffer.bytes &&
              buffers.snapshot().totals[1].destroyed == beforeBuffer.destroyed + 1,
              "AA final buffer-view release clears live capacity");
    }
}
#endif
} // namespace aa_memory_tests
