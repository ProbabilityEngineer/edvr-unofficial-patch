#pragma once

#include <windows.h>
#include "log.h"
#include "near_alloc_policy.h"

namespace edvr::near_alloc {

inline ConstrainedResult allocateConstrained(uintptr_t anchor, size_t bytes, const char* label) {
    using VirtualAlloc2Fn = PVOID (WINAPI *)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                            MEM_EXTENDED_PARAMETER*, ULONG);
    // Resolve from the loaded kernelbase, including CrossOver's ARM DLLs.
    // No load, new import dependency or native NT fallback is needed.
    const HMODULE module = GetModuleHandleW(L"kernelbase.dll");
    const auto allocate = module
        ? reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(module, "VirtualAlloc2")) : nullptr;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto window = addressWindow(anchor, bytes, info.dwAllocationGranularity, info.dwPageSize,
        reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress),
        reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress));
    DWORD error = ERROR_SUCCESS, releaseError = ERROR_SUCCESS;
    const auto result = tryConstrained(window, allocate != nullptr,
        [&](const AddressWindow& bounds) {
            MEM_ADDRESS_REQUIREMENTS requirements{};
            requirements.LowestStartingAddress = reinterpret_cast<void*>(bounds.low);
            requirements.HighestEndingAddress = reinterpret_cast<void*>(bounds.high);
            requirements.Alignment = bounds.alignment;
            MEM_EXTENDED_PARAMETER parameter{};
            parameter.Type = MemExtendedParameterAddressRequirements;
            parameter.Pointer = &requirements;
            void* memory = allocate(GetCurrentProcess(), nullptr, bounds.bytes,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1);
            if (!memory) error = GetLastError();
            return reinterpret_cast<uintptr_t>(memory);
        },
        [&](uintptr_t address) {
            const bool freed = VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE) != 0;
            if (!freed) releaseError = GetLastError();
            return freed;
        });
    const char* outcome = "unavailable";
    switch (result.state) {
    case ConstrainedState::InvalidWindow: outcome = "invalid-window"; break;
    case ConstrainedState::Failed: outcome = "failed"; break;
    case ConstrainedState::Success: outcome = "success"; break;
    case ConstrainedState::Rejected: outcome = "rejected"; break;
    case ConstrainedState::CleanupFailed: outcome = "cleanup-failed"; break;
    case ConstrainedState::Unavailable: break;
    }
    Log::get().note("near-alloc constrained: hook=%s api=VirtualAlloc2 outcome=%s "
                    "target=%p result=%p bytes=%zu rounded=%zu low=%p high=%p alignment=%zu "
                    "error=%lu release-error=%lu fallback=%u",
                    label, outcome, reinterpret_cast<void*>(anchor),
                    reinterpret_cast<void*>(result.address), bytes, window.bytes,
                    reinterpret_cast<void*>(window.low), reinterpret_cast<void*>(window.high),
                    window.alignment, error, releaseError,
                    unsigned(!result.address && !result.safetyRefusal()));
    SetLastError(releaseError ? releaseError : error);
    return result;
}

} // namespace edvr::near_alloc
