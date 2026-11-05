#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr::near_alloc {

struct AddressWindow {
    uintptr_t low = 0, high = 0; // Inclusive; the entire rounded allocation must fit.
    size_t bytes = 0, alignment = 0;
    bool valid = false;
};

inline AddressWindow addressWindow(uintptr_t anchor, size_t bytes, size_t alignment,
                                   size_t page, uintptr_t minimum, uintptr_t maximum) noexcept {
    constexpr uintptr_t reach = 0x7FF00000;
    if (!bytes || !page || (page & (page - 1)) || alignment < page ||
        (alignment & (alignment - 1)) || minimum > maximum ||
        bytes > UINTPTR_MAX - (page - 1)) return {};
    uintptr_t low = anchor > reach ? anchor - reach : 0;
    uintptr_t high = anchor > UINTPTR_MAX - reach ? UINTPTR_MAX : anchor + reach;
    if (low < minimum) low = minimum;
    if (high > maximum) high = maximum;
    if (low < alignment) low = alignment; // Never request the null granule.
    if (low > UINTPTR_MAX - (alignment - 1)) return {};
    low = (low + alignment - 1) & ~(uintptr_t(alignment) - 1);
    // Conservatively exclude the upper boundary's partial page.
    high &= ~(uintptr_t(page) - 1);
    if (!high) return {};
    --high;
    const size_t rounded = (bytes + page - 1) & ~(page - 1);
    if (low > high || rounded - 1 > high - low) return {};
    return {low, high, rounded, alignment, true};
}

enum class ConstrainedState { Unavailable, InvalidWindow, Failed, Success, Rejected, CleanupFailed };
struct ConstrainedResult {
    uintptr_t address = 0;
    ConstrainedState state = ConstrainedState::Unavailable;
    bool safetyRefusal() const noexcept {
        return state == ConstrainedState::InvalidWindow ||
               state == ConstrainedState::Rejected || state == ConstrainedState::CleanupFailed;
    }
};

// Callbacks make the production address/cleanup policy testable without a game
// or Windows process. Only ordinary unavailability/allocation failure falls back.
template<class Allocate, class Release>
ConstrainedResult tryConstrained(const AddressWindow& window, bool available,
                                 Allocate allocate, Release release) {
    if (!available) return {};
    if (!window.valid) return {0, ConstrainedState::InvalidWindow};
    const uintptr_t address = allocate(window);
    if (!address) return {0, ConstrainedState::Failed};
    if (address < window.low || address > window.high ||
        address % window.alignment || window.bytes - 1 > window.high - address) {
        const bool freed = release(address);
        return {0, freed ? ConstrainedState::Rejected : ConstrainedState::CleanupFailed};
    }
    return {address, ConstrainedState::Success};
}

} // namespace edvr::near_alloc
