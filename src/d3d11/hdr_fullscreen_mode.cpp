// fix.hdr's display-mode negotiation. hdr_fullscreen_mode.h says why this
// exists and what it refuses to do; this is EDHDR's src/fullscreen_mode.inc in
// EDVR's idiom, with its reasoning kept where it earned its place.
#include "hdr_fullscreen_mode.h"

#include <cstring>

#include "../common/log.h"
#include "../common/vtable_hook.h"
#include "hdr_output.h"
#include "hdr_output_rules.h"

namespace edvr {
namespace {

constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// IDXGIOutput1::FindClosestMatchingMode1. IDXGIObject takes 0..6, IDXGIOutput
// 7..18 (GetDesc through GetFrameStatistics), and IDXGIOutput1 adds
// GetDisplayModeList1 at 19 and FindClosestMatchingMode1 at 20.
constexpr size_t kOutputFindClosestMatchingMode1 = 20;

static_assert(hdrrules::kErrorNotFound == unsigned(DXGI_ERROR_NOT_FOUND),
              "the rules header's DXGI_ERROR_NOT_FOUND must be the real one");
static_assert(hdrrules::kRgba16Float == unsigned(kHdrFormat),
              "the rules header's FP16 code must be the format this creates");

// The policy a recorded swapchain carries. Private data follows COM object
// lifetime, including allocator reuse, so the cookie is load-bearing: a
// recycled allocation must not read as a recorded chain.
constexpr UINT kPolicyCookie = 0x45565231;  // "EVR1"
// {b7a1c4e2-5f3d-4a86-9c21-0d7e48b35f91}
const GUID kFullscreenModePolicy = {
    0xb7a1c4e2, 0x5f3d, 0x4a86, {0x9c, 0x21, 0x0d, 0x7e, 0x48, 0xb3, 0x5f, 0x91}};

struct Policy {
    UINT cookie;
    UINT requested;
    UINT created;
};

using PfnFindClosestMatchingMode1 = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput1*,
                                                                const DXGI_MODE_DESC1*,
                                                                DXGI_MODE_DESC1*, IUnknown*);
HRESULT STDMETHODCALLTYPE divertedMatch(IDXGIOutput1* output, const DXGI_MODE_DESC1* request,
                                        DXGI_MODE_DESC1* selected, IUnknown* device);

// One immutable copy per output vtable, for the life of the process. A
// concurrent reader may still hold a copy when a transition puts the original
// pointer back, so a copy is never freed and never rewritten.
struct Table {
    void** original = nullptr;
    void** copy = nullptr;
    PfnFindClosestMatchingMode1 match = nullptr;
};
Table g_tables[16];
volatile LONG g_tableCount = 0;
volatile LONG g_matchLogs = 0;
volatile LONG g_setLogs = 0;
SRWLOCK g_tableLock = SRWLOCK_INIT;
SRWLOCK g_transitionLock = SRWLOCK_INIT;

Table* tableFor(void** vtable, bool create) {
    const LONG count = InterlockedCompareExchange(&g_tableCount, 0, 0);
    for (LONG i = 0; i < count; ++i)
        if (g_tables[i].original == vtable || g_tables[i].copy == vtable) return &g_tables[i];
    if (!create) return nullptr;

    AcquireSRWLockExclusive(&g_tableLock);
    Table* result = nullptr;
    const LONG now = InterlockedCompareExchange(&g_tableCount, 0, 0);
    for (LONG i = 0; i < now; ++i)
        if (g_tables[i].original == vtable || g_tables[i].copy == vtable) {
            result = &g_tables[i];
            break;
        }
    if (!result && now < LONG(sizeof g_tables / sizeof g_tables[0])) {
        // The whole table is copied, not a prefix: an output here can be
        // IDXGIOutput6, and DXMT carries a private extension past that. A
        // truncated copy would turn one of those into a jump to nothing.
        const size_t slots = probeVTableLength(vtable, 64);
        if (slots > kOutputFindClosestMatchingMode1) {
            void** copy = static_cast<void**>(VirtualAlloc(
                nullptr, slots * sizeof(void*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (copy) {
                std::memcpy(copy, vtable, slots * sizeof(void*));
                copy[kOutputFindClosestMatchingMode1] = reinterpret_cast<void*>(&divertedMatch);
                g_tables[now].original = vtable;
                g_tables[now].copy = copy;
                g_tables[now].match = reinterpret_cast<PfnFindClosestMatchingMode1>(
                    vtable[kOutputFindClosestMatchingMode1]);
                result = &g_tables[now];
                InterlockedIncrement(&g_tableCount);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_tableLock);
    return result;
}

// True only for a swapchain this feature upgraded, which is still FP16 now.
bool recorded(IDXGISwapChain* swapChain, DXGI_SWAP_CHAIN_DESC* desc) {
    if (!swapChain || !hdrOutputOn()) return false;
    Policy policy = {};
    UINT size = sizeof policy;
    if (FAILED(swapChain->GetPrivateData(kFullscreenModePolicy, &size, &policy)) ||
        size != sizeof policy || policy.cookie != kPolicyCookie ||
        policy.created != UINT(kHdrFormat))
        return false;
    return SUCCEEDED(swapChain->GetDesc(desc)) && desc->BufferDesc.Format == kHdrFormat;
}

struct Scope;
thread_local Scope* t_active = nullptr;

// The diversion, for the duration of one display-mode transaction on one
// output, on this thread. Outside a scope the output's own vtable is in place
// and nothing is intercepted.
struct Scope {
    Scope* previous = nullptr;
    IDXGIOutput1* output = nullptr;
    Table* table = nullptr;
    void** previousTable = nullptr;
    bool locked = false;
    bool armed = false;

    Scope(IDXGISwapChain* swapChain, IDXGIOutput* explicitOutput, bool entering) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (!recorded(swapChain, &desc)) return;
        // A ResizeTarget while already windowed starts no transaction.
        if (!entering && desc.Windowed) return;
        // A window-mode setter can synchronously run another thread's window
        // procedure. That thread must never be made to wait on this lock, so
        // the lock is tried and the diversion simply skipped when it is held.
        if (!t_active) {
            if (!TryAcquireSRWLockExclusive(&g_transitionLock)) return;
            locked = true;
        }
        IDXGIOutput* selected = explicitOutput;
        if (!selected && FAILED(swapChain->GetContainingOutput(&selected))) return;
        if (!selected) return;
        const HRESULT hr =
            selected->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output));
        if (!explicitOutput) selected->Release();
        if (FAILED(hr) || !output) return;
        previousTable = *reinterpret_cast<void***>(output);
        table = tableFor(previousTable, true);
        if (!table) return;
        previous = t_active;
        t_active = this;
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(output), table->copy);
        armed = true;
    }

    ~Scope() {
        if (armed) {
            // Compare-exchange, not a store: something else may have installed
            // its own hook during the call, and that one is not ours to undo.
            InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(output),
                                              previousTable, table->copy);
            t_active = previous;
        }
        if (output) output->Release();
        if (locked) ReleaseSRWLockExclusive(&g_transitionLock);
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

HRESULT STDMETHODCALLTYPE divertedMatch(IDXGIOutput1* output, const DXGI_MODE_DESC1* request,
                                        DXGI_MODE_DESC1* selected, IUnknown* device) {
    Scope* scope = t_active;
    while (scope && (!scope->armed || scope->output != output)) scope = scope->previous;
    Table* table = scope ? scope->table : tableFor(*reinterpret_cast<void***>(output), false);
    if (!table || !table->match) return E_UNEXPECTED;

    const HRESULT hr = table->match(output, request, selected, device);
    // Native success and native validation failures are left exactly as they
    // are. The ONLY thing retried is the one observed failure: no display mode
    // for an FP16 request, inside this chain's own transaction.
    if (!hdrrules::retriesModeInEightBit(static_cast<unsigned>(hr), scope != nullptr,
                                         request ? unsigned(request->Format) : 0u,
                                         request != nullptr, selected != nullptr,
                                         device != nullptr))
        return hr;

    DXGI_MODE_DESC1 mode = *request;
    mode.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    const HRESULT retry = table->match(output, &mode, selected, device);
    const LONG n = InterlockedIncrement(&g_matchLogs);
    if (n <= 8)
        Log::get().note("hdr: display mode %ux%u was asked for as format %u, which has no "
                        "mode (0x%08lX); asked again as 28 and got 0x%08lX, %ux%u format %u. "
                        "The FP16 chain stores FP16; the DISPLAY still scans out 32 bpp, so "
                        "28 is the truthful mode and handing back %u would fail again.",
                        request->Width, request->Height, unsigned(kHdrFormat),
                        static_cast<unsigned long>(hr), static_cast<unsigned long>(retry),
                        SUCCEEDED(retry) ? selected->Width : 0,
                        SUCCEEDED(retry) ? selected->Height : 0,
                        SUCCEEDED(retry) ? unsigned(selected->Format) : 0, unsigned(kHdrFormat));
    else if (n == 9)
        Log::get().note("hdr: display mode matches go quiet here; they are being answered "
                        "the same way every time");
    return retry;
}

}  // namespace

void hdrModeRecord(IDXGISwapChain* swapChain, DXGI_FORMAT requested, DXGI_FORMAT created) {
    if (!swapChain || created != kHdrFormat) return;
    Policy policy = {kPolicyCookie, UINT(requested), UINT(created)};
    const HRESULT hr = swapChain->SetPrivateData(kFullscreenModePolicy, sizeof policy, &policy);
    if (FAILED(hr))
        Log::get().note("hdr: the swapchain would not carry its display-mode policy "
                        "(0x%08lX); fullscreen mode selection will be left to fail on its "
                        "own, as it did before this was handled",
                        static_cast<unsigned long>(hr));
}

HRESULT hdrModeSetFullscreenState(IDXGISwapChain* swapChain, void* original, BOOL fullscreen,
                                 IDXGIOutput* target) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
    const Fn forward = reinterpret_cast<Fn>(original);
    if (!forward) return E_UNEXPECTED;
    if (!fullscreen) return forward(swapChain, fullscreen, target);

    Scope scope(swapChain, target, true);
    // Without a target DXMT makes a fresh output of its own inside the call.
    // Resolving the real containing output at this event is what lets the
    // diversion above apply to the output the call will actually use -- and it
    // is handed over as itself, never wrapped in a proxy.
    IDXGIOutput* forwarded =
        (scope.armed && !target) ? static_cast<IDXGIOutput*>(scope.output) : target;
    const HRESULT hr = forward(swapChain, fullscreen, forwarded);
    if (scope.armed && InterlockedIncrement(&g_setLogs) <= 8)
        Log::get().note("hdr: fullscreen state %d on the FP16 chain, target %p (resolved from "
                        "%p), 0x%08lX", fullscreen, static_cast<void*>(forwarded),
                        static_cast<void*>(target), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT hdrModeResizeTarget(IDXGISwapChain* swapChain, void* original,
                            const DXGI_MODE_DESC* mode) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
    const Fn forward = reinterpret_cast<Fn>(original);
    if (!forward) return E_UNEXPECTED;
    if (!mode || (mode->Format != kHdrFormat && mode->Format != DXGI_FORMAT_UNKNOWN))
        return forward(swapChain, mode);
    Scope scope(swapChain, nullptr, false);
    return forward(swapChain, mode);
}

void hdrModeShutdown() {
    // The copies are deliberately not freed: a reader on another thread may
    // still hold one. They are one page each, at most sixteen, for the life of
    // the process. What is dropped is only the right to make more.
    InterlockedExchange(&g_matchLogs, 0);
    InterlockedExchange(&g_setLogs, 0);
}

}  // namespace edvr
