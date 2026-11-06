#pragma once

#include <atomic>
#include <cstdint>

namespace edvr::aa_memory {

// Private diagnostic build only; not a user setting. The control branch keeps
// record velocity enabled. The isolation branch changes only this constant.
inline constexpr bool kIsolateEngineRecords = true;
inline constexpr bool engineRecordsAllowed(bool flat, bool isolate = kIsolateEngineRecords) {
    return !flat || !isolate;
}

enum class Owner : unsigned { None, Engine, Foreground, Resolver, OtherEdvr, Count };
inline constexpr unsigned kOwners = unsigned(Owner::Count);
inline const char* name(Owner owner) {
    switch (owner) {
    case Owner::Engine: return "engine-motion";
    case Owner::Foreground: return "foreground";
    case Owner::Resolver: return "resolver";
    case Owner::OtherEdvr: return "other-edvr";
    default: return "none";
    }
}
inline thread_local Owner current = Owner::None;
class Scope {
public:
    explicit Scope(Owner owner) : previous_(current) { current = owner; }
    ~Scope() { current = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Owner previous_;
};
inline Owner allocationOwner(bool fromEdvr) {
    return current != Owner::None ? current : fromEdvr ? Owner::OtherEdvr : Owner::None;
}
template<class F> auto create(Owner owner, F&& operation) -> decltype(operation()) {
    Scope scope(owner);
    return operation();
}

enum class Work : unsigned {
    EngineAttempt, EngineSubstituted, DomainAttempt, DomainIssued,
    ForegroundCapture, ResolvePreflight, ResolveAttempt, FrameCleanup, Count
};
inline std::atomic<uint64_t> work[unsigned(Work::Count)]{};
inline void note(Work event) { work[unsigned(event)].fetch_add(1, std::memory_order_relaxed); }
inline uint64_t count(Work event) { return work[unsigned(event)].load(std::memory_order_relaxed); }

} // namespace edvr::aa_memory
