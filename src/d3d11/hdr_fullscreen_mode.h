// fix.hdr's display-mode negotiation.
//
// Storing FP16 in the swapchain and negotiating a DISPLAY MODE are different
// things, and only the first is EDVR's business. The second broke on the
// 2026-11-01 flight: with the swapchain created R16G16B16A16_FLOAT, Elite's
// fullscreen path asks the containing output to match a mode whose Format is
// that FP16 format, no such display mode exists, and the match returns
// DXGI_ERROR_NOT_FOUND -- several hundred times, because the game asks again
// every time it tries.
//
// EDHDR did not hit this because of what it did NOT do: it left the DXGI
// swapchain's declared format alone and had DXMT convert the surface
// underneath, so DXGI mode negotiation went on seeing an 8-bit chain. EDVR
// creates the swapchain itself and declares FP16, which is what makes the
// in-place UAV encode possible, so it has to answer for the mode question too.
//
// The answer here is EDHDR's own, ported from src/fullscreen_mode.inc, which
// is the form that flew: for the duration of a SetFullscreenState or a
// ResizeTarget on OUR chain, the containing output's FindClosestMatchingMode1
// is diverted; it forwards natively, and ONLY on the observed
// DXGI_ERROR_NOT_FOUND for an FP16 request does it retry the same mode as
// R8G8B8A8_UNORM and return that. The selected mode keeps format 28, which is
// the truthful display mode and what DXMT sets 32-bpp scanout from; handing
// back 10 would recreate the failure.
//
// What this deliberately does NOT do, because EDHDR's notes are explicit that
// each one made things worse: invent modes, fake fullscreen state, disguise a
// resource format, drive the geometry setters, or do any work in Present.
#pragma once

#include <d3d11.h>
#include <dxgi1_5.h>

namespace edvr {

// Record that this swapchain was created FP16 in place of the 8-bit format the
// game asked for. Only a chain recorded here is eligible for any of the
// behaviour below; everything else forwards untouched. Called from the
// swapchain-creation hook, with the format the game asked for.
void hdrModeRecord(IDXGISwapChain* swapChain, DXGI_FORMAT requested, DXGI_FORMAT created);

// The two swapchain methods that start a display-mode transaction. Each takes
// the original it must forward to. They are no-ops beyond forwarding unless
// fix.hdr is on and the chain was recorded above.
HRESULT hdrModeSetFullscreenState(IDXGISwapChain* swapChain, void* original, BOOL fullscreen,
                                  IDXGIOutput* target);
HRESULT hdrModeResizeTarget(IDXGISwapChain* swapChain, void* original,
                            const DXGI_MODE_DESC* mode);

// Drop the process-wide tables. Called from the device hook's shutdown.
void hdrModeShutdown();

}  // namespace edvr
