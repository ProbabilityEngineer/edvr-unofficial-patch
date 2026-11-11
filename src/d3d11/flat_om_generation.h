// One counter that moves on every output-merger state change EDVR can see.
//
// The forward-light watch (flat_forward_light_runtime.inl) asked the immediate context, on every watched draw, for
// the bound render targets, all 64 UAV slots, the blend state, the depth view and the depth-stencil state: about 350
// target reads and 520 depth reads a frame in a hangar (the flat cpu "copy checks" line, 2026-11-11), each one a
// call through the driver with a reference taken and dropped per slot. Between two draws those answers change only
// when something sets output-merger state, so the watch keeps its last answer and asks again only after a set.
//
// It moves at the top of every hooked setter (OMSetRenderTargets, OMSetRenderTargetsAndUnorderedAccessViews,
// OMSetBlendState, OMSetDepthStencilState), on ClearState, command lists and context-state swaps, and in EDVR's own
// raw setters that bypass the hooks (vscreen.cpp). It moves for every context and for EDVR's internal binds too:
// a spurious move costs one ask, a missed one would be a stale answer. The binding shadow's own render-target and
// depth generations (frame boundary, forget-all) are part of the same key at the reader, and the reader also checks
// a share of its answers against the context (flat_query_cut.h): a disagreement sends it back to asking for the
// session, with one log line.
#pragma once

#include <atomic>
#include <cstdint>

namespace edvr {

inline std::atomic<uint32_t> g_flatOmSets{1};
// Whether every setter above is hooked. OMSetDepthStencilState and SwapDeviceContextState sit past ClearState and exist
// only on a Context1 vtable (vscreen.cpp's issueSlot): without them a set could go unseen, and the reader asks every time.
inline std::atomic<bool> g_flatOmHooksComplete{false};

inline void flatOmSetNote() noexcept { g_flatOmSets.fetch_add(1, std::memory_order_relaxed); }
inline uint32_t flatOmSetGeneration() noexcept { return g_flatOmSets.load(std::memory_order_relaxed); }
inline void flatOmSetHooksComplete(bool complete) noexcept { g_flatOmHooksComplete.store(complete, std::memory_order_relaxed); }
inline bool flatOmSetTracked() noexcept { return g_flatOmHooksComplete.load(std::memory_order_relaxed); }

}  // namespace edvr
