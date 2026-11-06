#pragma once

#include <cstdint>
#include <mutex>

namespace edvr {

constexpr uint32_t kPresentTestFlag = 0x1u;
constexpr uint32_t kPresentOccluded = 0x087a0001u;

// A TEST is a status probe, not a frame boundary. Decide before invoking any
// rendering, timing queries, history maintenance or frame counters. Foreign
// chains also forward untouched and do not contribute to our diagnostics.
template<class Chain, class Forward, class Frame, class Observe>
auto dispatchPresent(Chain* chain, Chain* owned, uint32_t interval, uint32_t flags,
                     Forward forward, Frame frame, Observe observe)
    -> decltype(forward(chain, interval, flags)) {
    if (chain != owned) return forward(chain, interval, flags);
    const auto result = (flags & kPresentTestFlag)
        ? forward(chain, interval, flags)
        : frame(chain, interval, flags);
    observe(flags, result);
    return result;
}

struct PresentStatusSnapshot {
    uint64_t tests = 0, regular = 0;
    uint64_t testOccluded = 0, regularOccluded = 0;
    unsigned report = 0;
};

// One initial line (including zeros), then at most one every 30 seconds,
// at most 120 lines per session. The final line explicitly says reporting
// stops. No timer thread, GPU access, or changes to the frame lifecycle.
class PresentStatusCounters {
public:
    static constexpr uint64_t kReportMs = 30000;
    static constexpr unsigned kMaxReports = 120;

    bool note(uint32_t flags, uint32_t result, uint64_t now, PresentStatusSnapshot& out) {
        // A status probe can arrive on a different thread. Keep the snapshot
        // coherent, but never hold this lock across Present or log output.
        std::lock_guard<std::mutex> lock(mutex_);
        const bool test = (flags & kPresentTestFlag) != 0;
        if (test) ++counts_.tests; else ++counts_.regular;
        if (result == kPresentOccluded) {
            if (test) ++counts_.testOccluded; else ++counts_.regularOccluded;
        }
        if (counts_.report >= kMaxReports ||
            (counts_.report && now - lastReport_ < kReportMs))
            return false;
        lastReport_ = now;
        ++counts_.report;
        out = counts_;
        return true;
    }

private:
    std::mutex mutex_;
    PresentStatusSnapshot counts_{};
    uint64_t lastReport_ = 0;
};

} // namespace edvr
