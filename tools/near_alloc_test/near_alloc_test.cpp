// Synthetic code only: no game, executable discovery or live hooks.
#include "../../src/common/code_hook.h"
#include "../../src/common/near_alloc_constrained.h"
#include <cstdio>
#include <cstring>

namespace edvr {
Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char*, ...) {}
int guardFilter(unsigned long, const char*) { return EXCEPTION_EXECUTE_HANDLER; }
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test")) return 2;
    unsigned failures = 0;
    const auto check = [&](bool ok, const char* name) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
    };
    using namespace edvr;
    const auto window = near_alloc::addressWindow(0x140000000, 65536, 65536, 4096,
                                                 65536, 0x7fffffff0000);
    check(window.valid && window.bytes == 65536, "whole shared block fits constrained window");
    unsigned calls = 0, frees = 0;
    auto result = near_alloc::tryConstrained(window, false,
        [&](const auto&) { ++calls; return uintptr_t(0); },
        [&](uintptr_t) { ++frees; return true; });
    check(!result.address && !result.safetyRefusal() && !calls, "unavailable API allows fixed fallback");
    result = near_alloc::tryConstrained(window, true,
        [&](const auto&) { ++calls; return uintptr_t(0); },
        [&](uintptr_t) { ++frees; return true; });
    check(!result.address && !result.safetyRefusal(), "ordinary failure allows fixed fallback");
    result = near_alloc::tryConstrained(window, true,
        [&](const auto&) { return window.low + 1; },
        [&](uintptr_t) { ++frees; return false; });
    check(result.safetyRefusal() && !result.address && frees == 1,
          "invalid placement and failed cleanup forbid fallback");

    SYSTEM_INFO layout{};
    GetSystemInfo(&layout);
    auto* block = static_cast<uint8_t*>(VirtualAlloc(nullptr, layout.dwAllocationGranularity,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(block != nullptr, "one synthetic reservation");
    if (!block) return 1;
    // Target and replacement share the first page; trampoline uses the last.
    const uint8_t original[] = {0x48,0x83,0xec,0x28,0x90,0x90,0x90,0x90};
    std::memcpy(block, original, sizeof(original));
    auto* tail = block + layout.dwAllocationGranularity - layout.dwPageSize;
    CodeHook::BorrowedTrampoline storage{tail, layout.dwPageSize};
    CodeHook hook;
    const auto refuse = [](void*, void*) noexcept { return false; };
    check(!hook.install(block, block + 64, nullptr, "refusal", refuse, nullptr, &storage),
          "prepare refusal does not install");
    MEMORY_BASIC_INFORMATION info{};
    check(VirtualQuery(tail, &info, sizeof(info)) && info.State == MEM_COMMIT &&
          info.AllocationBase == block, "failure leaves borrowed storage allocated");
    check(std::memcmp(block, original, sizeof(original)) == 0, "failure leaves target untouched");
    DWORD old = 0;
    check(VirtualProtect(tail, layout.dwPageSize, PAGE_READWRITE, &old) != 0, "reset test page");
    const auto prepare = [](void*, void* context) noexcept {
        static_cast<uint8_t*>(context)[64] = 0xc3;
        return true;
    };
    void* forward = nullptr;
    check(hook.install(block, block + 64, &forward, "shared", prepare, block, &storage) &&
          forward == tail, "relay stays writable after trampoline protection, no second allocation");
    hook.uninstall();
    check(std::memcmp(block, original, sizeof(original)) == 0, "uninstall restores target");
    check(VirtualQuery(tail, &info, sizeof(info)) && info.State == MEM_COMMIT &&
          info.AllocationBase == block, "uninstall leaves shared block owned by caller");
    check(VirtualFree(block, 0, MEM_RELEASE) != 0, "owner releases full block once");
    std::printf("near_alloc_test: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
