#pragma once

#include "aa_memory_scope.h"
#include "texture_lifetime_diagnostics.h"

namespace edvr::aa_memory {

// Different from the original texture tag: both observations must survive on
// the same object. A tag never owns the resource it describes.
inline const GUID kOwnerTagGuid =
    {0xb550c0c4, 0x26aa, 0x46dc, {0xb8, 0xf1, 0x9b, 0x2d, 0x5a, 0x1e, 0x98, 0x70}};
struct State {
    // One ledger per scope/kind; descriptor overflow cannot lose owner totals.
    texture_lifetime::Ledger ledger[kOwners][2];
};
inline State* state() {
    auto* lifetime = texture_lifetime::diagnosticState();
    if (!lifetime || !lifetime->pinned) return nullptr;
    static State* value = new (std::nothrow) State;
    return value; // intentionally process-lived, like the original tag ledger
}
template<class Resource>
void observe(Resource* resource, bool buffer, bool fromEdvr, uint64_t bytes) {
    const Owner owner = allocationOwner(fromEdvr);
    if (owner == Owner::None) return;
    auto* s = state();
    if (!s) return; // report() makes unavailable coverage visible
    texture_lifetime::Key key{};
    key.edvr = true;
    texture_lifetime::attach(s->ledger[unsigned(owner)][unsigned(buffer)],
        resource, key, bytes, kOwnerTagGuid);
}

inline void report(unsigned sample, unsigned maxSamples, bool engineActive,
                   uint64_t pinnedCount, uint64_t pinnedBytes) {
    auto* s = state();
    if (!s) {
        Log::get().note("aa memory: enabled=0 reason=state-or-module-pin-unavailable sample=%u/%u",
            sample, maxSamples);
        return;
    }
    if (sample == 1)
        Log::get().note("aa memory: coverage=hooked-buffer-and-texture2d allocation-scope-not-retaining-owner=1 "
            "excludes=pre-hook,swapchain,other-entrypoints,unscoped-external "
            "buffer-bytes=ByteWidth-including-unused-capacity texture-bytes=estimated "
            "tag-cap-per-owner-kind=%llu "
            "physical-backing-not-measured=1 pooled-resources-counted-while-alive=1 "
            "owner-textures-subset-of-texture-lifetime-do-not-add=1 "
            "engine-isolation=record-velocity-only domain-markers-remain-independent",
            (unsigned long long)texture_lifetime::kMaxTags);
    Log::get().note("aa memory: sample=%u/%u isolate-engine-records=%u engine-active=%u bypass-map-bounce=%u "
        "pinned-game-buffers=%llu pinned-game-buffer-bytes=%llu "
        "engine-attempts=%llu engine-substituted=%llu domain-attempts=%llu domain-issued=%llu "
        "foreground-capture-attempts=%llu resolve-preflights=%llu resolve-attempts=%llu frame-cleanups=%llu",
        sample, maxSamples, unsigned(kIsolateEngineRecords), unsigned(engineActive), unsigned(kBypassMapBounce),
        (unsigned long long)pinnedCount, (unsigned long long)pinnedBytes,
        (unsigned long long)count(Work::EngineAttempt), (unsigned long long)count(Work::EngineSubstituted),
        (unsigned long long)count(Work::DomainAttempt), (unsigned long long)count(Work::DomainIssued),
        (unsigned long long)count(Work::ForegroundCapture), (unsigned long long)count(Work::ResolvePreflight),
        (unsigned long long)count(Work::ResolveAttempt), (unsigned long long)count(Work::FrameCleanup));
    for (unsigned owner = 1; owner < kOwners; ++owner) for (unsigned buffer = 0; buffer < 2; ++buffer) {
        const auto now = s->ledger[owner][buffer].snapshot();
        const auto& c = now.totals[1];
        Log::get().note("aa memory owner: owner=%s kind=%s observed=%llu attach-failed=%llu cap-skipped=%llu pending=%llu "
            "created=%llu destroyed=%llu live=%llu peak=%llu live-bytes=%llu peak-bytes=%llu",
            name(Owner(owner)), buffer ? "buffer" : "texture2d",
            (unsigned long long)now.observed, (unsigned long long)now.failed,
            (unsigned long long)now.capped, (unsigned long long)now.pending,
            (unsigned long long)c.created, (unsigned long long)c.destroyed,
            (unsigned long long)c.live, (unsigned long long)c.peak,
            (unsigned long long)c.bytes, (unsigned long long)c.peakBytes);
    }
}

} // namespace edvr::aa_memory
