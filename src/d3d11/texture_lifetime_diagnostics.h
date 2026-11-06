#pragma once

#include "texture_lifetime_d3d11.h"
#include "../common/log.h"

namespace edvr::texture_lifetime {

struct DiagnosticState {
    Ledger ledger;
    std::mutex reportMutex;
    std::array<uint64_t, kGroups> previous{};
    bool pinned = false;
};

inline DiagnosticState* diagnosticState() {
    // Tags can die on driver threads late in teardown. Neither their ledger
    // nor their executable code may be destroyed/unloaded first. This private
    // diagnostic candidate deliberately pins its module until process exit.
    static DiagnosticState* state = [] {
        auto* s = new (std::nothrow) DiagnosticState;
        if (s) {
            HMODULE module = nullptr;
            s->pinned = GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&diagnosticState), &module) != FALSE;
        }
        return s;
    }();
    return state;
}

inline void observe(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& desc,
                    bool fromEdvr, uint64_t estimatedBytes) {
    auto* state = diagnosticState();
    if (state && state->pinned)
        attach(state->ledger, texture, keyFor(desc, fromEdvr), estimatedBytes);
}

// Called only when the existing bounded Present-status reporter emits.
// No GPU queries, waits, readbacks, or driver calls in this reporting path.
inline void report(unsigned sample, unsigned maxSamples) {
    auto* state = diagnosticState();
    if (!state || !state->pinned) {
        Log::get().note("texture lifetime: enabled=0 reason=%s sample=%u/%u",
            state ? "module-pin-failed" : "state-allocation-failed", sample, maxSamples);
        return;
    }
    std::lock_guard<std::mutex> reportLock(state->reportMutex);
    const Snapshot now = state->ledger.snapshot();
    if (sample == 1)
        Log::get().note("texture lifetime: diagnostic=1 module-pinned=1 "
            "coverage=hooked-CreateTexture2D-only origin=caller-image "
            "excludes=pre-hook,swapchain,other-entrypoints bytes=estimated-not-physical "
            "descriptor-slots=%u tag-cap=%llu no-texture-reference-held=1",
            kDescriptors, static_cast<unsigned long long>(kMaxTags));
    Log::get().note("texture lifetime: sample=%u/%u observed=%llu attach-failed=%llu "
        "cap-skipped=%llu pending=%llu%s",
        sample, maxSamples, static_cast<unsigned long long>(now.observed),
        static_cast<unsigned long long>(now.failed), static_cast<unsigned long long>(now.capped),
        static_cast<unsigned long long>(now.pending),
        sample == maxSamples ? " reporting-stopped=session-cap" : "");
    for (unsigned origin = 0; origin < 2; ++origin) {
        const auto& c = now.totals[origin];
        Log::get().note("texture lifetime total: origin=%s created=%llu destroyed=%llu "
            "live=%llu peak=%llu live-est-bytes=%llu peak-est-bytes=%llu",
            origin ? "edvr" : "external",
            static_cast<unsigned long long>(c.created), static_cast<unsigned long long>(c.destroyed),
            static_cast<unsigned long long>(c.live), static_cast<unsigned long long>(c.peak),
            static_cast<unsigned long long>(c.bytes), static_cast<unsigned long long>(c.peakBytes));
    }
    for (unsigned i : reportGroups(now, state->previous)) {
        if (i == kInvalid) continue;
        const auto& g = now.groups[i];
        const auto& d = g.key.desc;
        const auto& c = g.counts;
        Log::get().note("texture lifetime group: slot=%u overflow=%u origin=%s "
            "size=%ux%u format=%u bind=0x%X mips=%u array=%u samples=%u quality=%u "
            "usage=%u cpu=0x%X misc=0x%X created=%llu destroyed=%llu live=%llu "
            "live-est-bytes=%llu delta-est-bytes=%lld peak-est-bytes=%llu",
            i, unsigned(i >= kDescriptors), g.key.edvr ? "edvr" : "external",
            d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10],
            static_cast<unsigned long long>(c.created), static_cast<unsigned long long>(c.destroyed),
            static_cast<unsigned long long>(c.live), static_cast<unsigned long long>(c.bytes),
            static_cast<long long>(c.bytes) - static_cast<long long>(state->previous[i]),
            static_cast<unsigned long long>(c.peakBytes));
    }
    for (unsigned i = 0; i < kGroups; ++i)
        state->previous[i] = now.groups[i].counts.bytes;
}

} // namespace edvr::texture_lifetime
