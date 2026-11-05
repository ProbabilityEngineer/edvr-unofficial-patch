// fix.hdr -- HDR output, inside EDVR instead of beside it.
//
// WHAT THIS REPLACES
//
// HDR on this setup has been a second d3d11 proxy, EDHDR, loaded in front of
// EDVR and chained to it (`CHAIN_D3D11=d3d11_edvr.dll`). Elite -> EDHDR ->
// EDVR -> DXMT. EDHDR owned five things, and every one of them is a decision
// about a surface EDVR also owns:
//
//   * the swapchain's format, forced to R16G16B16A16_FLOAT;
//   * an RTV compatibility rewrite, because Elite still makes an 8-bit RTV
//     over what is now an FP16 backbuffer, and that mismatch killed the
//     device;
//   * the tone pixel shader, substituted so the game writes display-referred
//     HDR instead of clamped SDR;
//   * somewhere wider than 8 bits for that tone output to land, which EDHDR
//     did by cloning every eligible RGBA8-typeless target to FP16 and then
//     healing every bind, every clear and every view that pointed at the
//     original;
//   * the final transform into the presented domain, PQ/BT.2020 or linear
//     scRGB, with the colour-space tag to match.
//
// Two proxies both patching the device, the factory, the swapchain and the
// SAME immediate context is the cost that made this worth doing. EDHDR
// publishes a private context vtable by pointer swap, so every bind EDVR
// itself issues -- and EDVR issues a great many -- went through EDHDR's
// forwarding lookup first. And the clone chain means the game's clears are
// issued twice, once to the original and once to the clone, for the whole
// session.
//
// WHAT CHANGES IN THE UNIFIED FORM
//
// EDVR already hooks every entry point this needs: CreateSwapChain and
// CreateSwapChainForHwnd on the factory, ResizeBuffers and Present on the
// swapchain, CreateTexture2D, CreatePixelShader and the three view creates on
// the device. So fix.hdr adds NO new interception of any kind. In particular
// it adds no context hooks, and the clone chain is gone:
//
//   * the widening happens IN PLACE, in hookedCreateTexture2D, by handing
//     D3D11 a wider desc than the game asked for. That is what the oracle
//     this work was fitted against (RenoDX) does -- an upgraded resource, not
//     a shadow of one. One resource instead of two means no bind-time heal,
//     no replayed clears, no 512-entry map, and CopyResource between the
//     game's own textures keeps working, which was a documented gap in the
//     clone chain.
//   * the output transform runs in place over the backbuffer as one compute
//     dispatch, because EDVR creates the swapchain and can therefore ask for
//     DXGI_USAGE_UNORDERED_ACCESS itself. EDHDR could not: it had to hope the
//     usage was already there, and fall back to a scratch copy and a
//     fullscreen draw. The fallback is kept here for exactly that case.
//
// WHERE IT SITS IN THE FRAME
//
// The transform is the last thing before the real Present, after the game's
// own 2D interface and after EDVR's F8 menu. It has to be: the game draws its
// GUI onto the backbuffer AFTER the final copy, so an encode done any earlier
// would miss it. Everything upstream of that point -- the tone shader, the
// upscale, the HUD layer, the composite, the copy -- composes in the
// display-referred gamma-2.2 domain the tone shader writes, which is why the
// GUI blends the way it always did.
//
// WHAT IS NOT CLAIMED
//
// This is a port, not a tuning. The shaders are EDHDR's text unedited
// (hdr_output_shaders.h) and the nits constants are the oracle's. The
// composition domain is still FP16, so the full-resolution surfaces EDVR
// copies and composites are still 8 bytes a pixel, and that -- not the
// proxy chain -- is where the measured GPU power went
// (docs/edvr-unified-hdr-2026-11-01.md). Reaching the SDR baseline's watts
// would need a 10-bit encoded composition domain, which moves where the
// game's GUI blends. That is a separate step and it is not in here.
#pragma once

#include <cstddef>

#include <d3d11.h>
#include <dxgi1_2.h>

namespace edvr {

// Read fix.hdr and advanced.hdr_encode once, and say what was decided. Called
// from the device hook's startup, before any swapchain exists, because the
// swapchain's format is the first thing that depends on the answer.
void hdrOutputStartup();

// True when fix.hdr is on. Every hook site below is a no-op when it is off,
// so the DLL behaves exactly as it does without this feature.
bool hdrOutputOn();

// The swapchain the game asked for, widened. Returns true when `out` was
// filled with a desc to use instead of the game's: FP16 format, and
// UNORDERED_ACCESS added to the buffer usage so the output transform can run
// in place. False means forward the game's desc untouched.
bool hdrOutputSwapChainDesc(const DXGI_SWAP_CHAIN_DESC& asked, DXGI_SWAP_CHAIN_DESC* out);
bool hdrOutputSwapChainDesc1(const DXGI_SWAP_CHAIN_DESC1& asked, DXGI_SWAP_CHAIN_DESC1* out);

// The format to pass to ResizeBuffers in place of the caller's. A resize
// takes the buffers back to the format named here, so an unwidened resize
// would drop the whole frame's domain. Returns the caller's format unchanged
// when the feature is off.
DXGI_FORMAT hdrOutputResizeFormat(DXGI_FORMAT asked);

// The buffers were resized (or the swapchain released its backbuffer): drop
// the cached backbuffer, views and scratch, and re-apply the colour-space tag
// on the next Present. DXMT reinitialises the FP16 layer as G10/P709 on a
// resize, so the tag does not survive one.
void hdrOutputSwapChainChanged();

// The game's texture, widened. Returns true when `out` holds a desc to create
// instead: R8G8B8A8_TYPELESS with a render-target or unordered-access bind
// becomes R16G16B16A16_TYPELESS, which is the oracle's upgrade rule. The tone
// shader's output reaches about 2.27 in encoded units, so an 8-bit target
// clamps the HDR away before the next pass can read it.
bool hdrOutputWidenTexture(const D3D11_TEXTURE2D_DESC& asked, D3D11_TEXTURE2D_DESC* out);

// The view format to use over a resource this feature widened. Returns true
// when `mapped` holds a format to use instead of `asked`: an 8-bit RGBA view
// over a 16-bit-float resource reads as R16G16B16A16_FLOAT. Stateless -- it
// asks the resource what it is -- so it covers both the widened game targets
// and Elite's 8-bit RTV over the FP16 backbuffer, which is the mismatch that
// used to remove the device.
bool hdrOutputViewFormat(ID3D11Resource* resource, DXGI_FORMAT asked, DXGI_FORMAT* mapped);

// The view desc to pass on, for the three view creates. Returns `desc`
// unchanged in the ordinary case, or `scratch` holding a copy of it with the
// format replaced. `descBytes` is the caller's desc size and `scratch` must be
// at least that large. One implementation serves the render-target, shader-
// resource and unordered-access descs because DXGI_FORMAT Format is the first
// member of all three; the caller asserts that.
const void* hdrOutputViewDescFor(ID3D11Resource* resource, const void* desc, void* scratch,
                                 size_t descBytes);

// Compile the HDR shaders (including both tone variants) and remember the device. Called once from the
// device hook's attach, not from the first Present: the tone and copy
// substitutions have to be ready before the game creates its pixel shaders
// during loading.
void hdrOutputAttach(ID3D11Device* device);

// A replacement for the pixel shader the game is creating, or null to let its
// own bytecode through. Matched on length, CRC-32 and FNV-1a-64 together, the
// way EDHDR matched them, so a partial match never substitutes. The returned
// shader is reference-counted for the caller to hand back to the game; the
// caller must still register the ORIGINAL hash for it, so every recogniser in
// EDVR goes on seeing the draw it expects.
ID3D11PixelShader* hdrOutputReplacementPs(const void* bytecode, size_t length);

// Apply the output domain and transform, immediately before the real Present
// and after the game's interface and EDVR's menu have been drawn. Fail-soft
// at every step: an unavailable colour space falls back to linear scRGB, a
// backbuffer that will not take a UAV falls back to the scratch copy and a
// fullscreen draw, and a failure of both leaves the frame untransformed and
// says so once.
void hdrOutputBeforePresent(IDXGISwapChain* swapChain, ID3D11DeviceContext* context);

// Release everything. Called from the device hook's shutdown.
void hdrOutputShutdown();

}  // namespace edvr
