#include "code_hook.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "guard.h"
#include "log.h"

namespace edvr {

namespace {

// The instruction shapes this decoder will move, and nothing else.
//
// A compiled x86-64 function almost always opens with some arrangement of:
// saving a parameter register into its shadow slot, pushing a callee-saved
// register, subtracting the frame size from rsp, or moving one register into
// another. Those are what is listed here. Everything else -- and every relative
// operand of any length -- is refused, because a hook that relocates an
// instruction it has misread does not fail here, it fails in somebody else's
// code with EDVR nowhere on the stack.
//
// The returned length counts the whole instruction including prefixes.

bool isRexPrefix(uint8_t b) { return b >= 0x40 && b <= 0x4F; }

// ModRM displacement size, and whether the operand is rip-relative (mod == 00
// and rm == 101, which on x86-64 means [rip+disp32] and is NOT relocatable).
bool modrmSize(uint8_t modrm, bool* ripRelative, size_t* extra) {
    const uint8_t mod = static_cast<uint8_t>(modrm >> 6);
    const uint8_t rm = static_cast<uint8_t>(modrm & 7);
    *ripRelative = false;
    *extra = 1;  // the ModRM byte itself
    if (mod == 3) return true;              // register operand, nothing follows
    if (rm == 4) *extra += 1;               // SIB
    if (mod == 0) {
        if (rm == 5) { *ripRelative = true; *extra += 4; }
        // A SIB with base == 101 also carries a disp32, which this decoder does
        // not attempt to read; refuse rather than guess.
        else if (rm == 4) return false;
    } else if (mod == 1) {
        *extra += 1;                        // disp8
    } else if (mod == 2) {
        *extra += 4;                        // disp32
    }
    return true;
}

}  // namespace

size_t codeInstructionLength(const uint8_t* code, size_t available,
                             size_t* ripDispOffset) {
    if (ripDispOffset) *ripDispOffset = 0;
    if (!code || available == 0) return 0;
    size_t i = 0;

    // Prefixes. Only the operand-size prefix and REX are expected in a
    // prologue; a segment, lock or rep prefix means this is not the sort of
    // instruction being decoded and the answer is 0.
    // F212: a scalar-single move (movss, F3 0F 10 / 0F 11) is the one F3-prefixed form understood, and only that: the prefix is taken here, and the opcode switch below
    // answers 0 for it unless it is followed by 0F 10 or 0F 11. A leaf that opens with a movss store (the look springs' target setter, 0x1A8DF40 in build 334925) is
    // otherwise unhookable.
    bool scalarSingle = false;
    if (i < available && code[i] == 0xF3) {
        scalarSingle = true;
        ++i;
    }
    if (!scalarSingle && i < available && code[i] == 0x66) ++i;
    if (i < available && isRexPrefix(code[i])) ++i;
    if (i >= available) return 0;

    const uint8_t op = code[i++];
    if (scalarSingle && !(op == 0x0F && i < available && (code[i] == 0x10 || code[i] == 0x11))) return 0;
    switch (op) {
        // push/pop r64 -- the classic prologue register saves.
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            return i;

        // mov r/m, r ; mov r, r/m ; and the 8-bit forms. This is
        // `mov [rsp+8], rcx` and `mov rbp, rsp`.
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        // lea r, m -- common, and refused below when rip-relative.
        case 0x8D:
        // add/or/and/sub/xor/cmp r/m, r and the reverse direction.
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        // inc/dec r/m and the group-5 forms. FF 05 is `inc [rip+disp32]`, how a
        // compiler increments a global -- and how the very first function this
        // class was ever pointed at happened to begin.
        case 0xFE: case 0xFF:
        case 0x84: case 0x85: {
            if (i >= available) return 0;
            bool rip = false;
            size_t extra = 0;
            if (!modrmSize(code[i], &rip, &extra)) return 0;
            // The displacement sits straight after the ModRM byte: the
            // rip-relative form is mod == 00, rm == 101, which excludes a SIB.
            if (rip && ripDispOffset) *ripDispOffset = i + 1;
            i += extra;
            return i <= available ? i : 0;
        }

        // Group 1 with an 8-bit immediate: `sub rsp, 0x28` and its relatives.
        case 0x83: {
            if (i >= available) return 0;
            bool rip = false;
            size_t extra = 0;
            if (!modrmSize(code[i], &rip, &extra)) return 0;
            if (rip && ripDispOffset) *ripDispOffset = i + 1;
            i += extra + 1;
            return i <= available ? i : 0;
        }

        // Group 1 with a 32-bit immediate: a large frame.
        case 0x81: {
            if (i >= available) return 0;
            bool rip = false;
            size_t extra = 0;
            if (!modrmSize(code[i], &rip, &extra)) return 0;
            if (rip && ripDispOffset) *ripDispOffset = i + 1;
            i += extra + 4;
            return i <= available ? i : 0;
        }

        // mov r32/r64, imm32 (B8+r). Ten bytes with REX.W as imm64, which is
        // the shape the D3D11 restore block itself uses.
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            const bool wide = i >= 2 && isRexPrefix(code[i - 2]) &&
                              (code[i - 2] & 0x08) != 0;
            i += wide ? 8 : 4;
            return i <= available ? i : 0;
        }

        // A one-byte no-op, and the multi-byte form compilers pad with.
        case 0x90:
            return i;
        case 0x0F: {
            if (i < available && code[i] == 0x1F) {
                ++i;
                if (i >= available) return 0;
                bool rip = false;
                size_t extra = 0;
                if (!modrmSize(code[i], &rip, &extra)) return 0;
                if (rip && ripDispOffset) *ripDispOffset = i + 1;
                i += extra;
                return i <= available ? i : 0;
            }
            // movups xmm, xmm/m128 (0F 10) and its store (0F 11): a plain unaligned move with a ModRM operand and no immediate, which is
            // how MSVC opens a leaf that copies a matrix (the SRV radar's placement setter, 0x38563A0 in build 334925, begins
            // `sub rsp,48h; movups xmm0,[rdx]`); with the F3 prefix (taken above) the same two opcodes are movss, which is how a leaf that
            // stores a float opens (F212: 0x1A8DF40). Moved as is unless rip-relative, which is measured like every other ModRM form.
            // Every other SSE form is still refused.
            if (i < available && (code[i] == 0x10 || code[i] == 0x11)) {
                ++i;
                if (i >= available) return 0;
                bool rip = false;
                size_t extra = 0;
                if (!modrmSize(code[i], &rip, &extra)) return 0;
                if (rip && ripDispOffset) *ripDispOffset = i + 1;
                i += extra;
                return i <= available ? i : 0;
            }
            return 0;   // every other two-byte opcode, including jcc rel32
        }

        default:
            // Everything unlisted, which includes E8 (call), E9/EB (jmp),
            // 7x (jcc rel8), C3 (ret) and every SSE form but movups and movss
            // (0F 10 / 0F 11 above). A prologue that begins with any of them is one
            // this class will not touch.
            return 0;
    }
}

namespace {

// Two gigabytes minus a margin, because a displacement is measured from the END of the instruction that holds it and the
// trampoline's own length has to fit inside the window too.
constexpr uintptr_t kReach = 0x7FF00000;

// WHERE A TRAMPOLINE GOES (2026-11-12, step 4 of the flat performance work).
//
// The patch at the target jumps straight to the REPLACEMENT; the trampoline is only ever reached through the pointer
// install() hands back, and it leaves by an absolute jump. So the trampoline itself has to be within reach of nothing --
// except when a moved instruction is rip-relative: its displacement is rewritten to address the same data from the
// trampoline, and that only fits in 32 bits if the trampoline is within two gigabytes of the data. Until now every
// trampoline was placed within two gigabytes of its target, and each took a whole allocation (64 KB of address space
// for a few dozen bytes). Near Elite's image that space is scarce under Wine and FEX: on 2026-11-12 three launches in a
// row ran out after four of the nine engine-motion hooks, and the temporal AA stood down.
//
// Now: a trampoline with no rip-relative instruction is allocated anywhere. One that needs to be near takes a PAGE
// from a pool of regions reserved near the main module -- the first at DLL load, before the game, Wine and FEX have
// filled the space (codeHookReserveNear) -- so a region of 64 KB holds sixteen trampolines, not one. Each trampoline
// still has a page of its own, so the old rule stands: a trampoline page is never writable and executable at once,
// and building one never touches a page another thread may be executing.
uintptr_t allocationGranularity() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
}
uintptr_t pageSize() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwPageSize ? si.dwPageSize : 0x1000;
}

// Reserve (not commit) one region within reach of `anchor`, walking outward from it a region at a time; MEM_RESERVE
// fails harmlessly on anything already taken.
uint8_t* reserveNear(uintptr_t at, size_t bytes) {
    const uintptr_t granularity = allocationGranularity();
    for (uintptr_t delta = granularity; delta < kReach - bytes; delta += granularity) {
        for (int direction = 0; direction < 2; ++direction) {
            const uintptr_t candidate = direction ? (at + delta) : (at - delta);
            if (direction == 0 && delta > at) continue;   // would wrap below zero
            void* const got = VirtualAlloc(reinterpret_cast<void*>(candidate & ~(granularity - 1)), bytes, MEM_RESERVE,
                                           PAGE_NOACCESS);
            if (got) return static_cast<uint8_t*>(got);
        }
    }
    return nullptr;
}

struct NearRegion {
    uint8_t* base = nullptr;
    unsigned pages = 0;
    uint64_t used = 0;   // one bit a page
};
constexpr unsigned kMaxRegions = 32;
SRWLOCK g_poolLock = SRWLOCK_INIT;
NearRegion g_regions[kMaxRegions];
unsigned g_regionCount = 0;

bool within(uintptr_t a, uintptr_t b, uintptr_t span) {
    const uintptr_t d = a > b ? a - b : b - a;
    return d + span < kReach;
}

// Under g_poolLock. A new region near `at`, or null.
NearRegion* addRegion(uintptr_t at) {
    if (g_regionCount == kMaxRegions) return nullptr;
    const uintptr_t region = allocationGranularity();
    const uintptr_t page = pageSize();
    uint8_t* const base = reserveNear(at, region);
    if (!base) return nullptr;
    NearRegion& r = g_regions[g_regionCount++];
    r.base = base;
    r.pages = static_cast<unsigned>(region / page < 64 ? region / page : 64);
    r.used = 0;
    return &r;
}

// A committed read-write page within reach of `at` (from the pool, reserving a region if none has room), or null.
uint8_t* poolTake(uintptr_t at) {
    const uintptr_t page = pageSize();
    AcquireSRWLockExclusive(&g_poolLock);
    uint8_t* got = nullptr;
    auto takeFrom = [&](NearRegion& r) {
        if (!within(reinterpret_cast<uintptr_t>(r.base), at, r.pages * page)) return false;
        for (unsigned i = 0; i < r.pages; ++i) {
            if (r.used & (uint64_t(1) << i)) continue;
            void* const p = VirtualAlloc(r.base + i * page, page, MEM_COMMIT, PAGE_READWRITE);
            if (!p) return false;
            r.used |= uint64_t(1) << i;
            got = static_cast<uint8_t*>(p);
            return true;
        }
        return false;
    };
    for (unsigned i = 0; i < g_regionCount && !got; ++i) takeFrom(g_regions[i]);
    if (!got) {
        if (NearRegion* r = addRegion(at)) takeFrom(*r);
    }
    ReleaseSRWLockExclusive(&g_poolLock);
    return got;
}

// True when `p` was a pool page: decommitted (so nothing is left executable there) and free for the next trampoline.
bool poolGive(uint8_t* p) {
    const uintptr_t page = pageSize();
    bool mine = false;
    AcquireSRWLockExclusive(&g_poolLock);
    for (unsigned i = 0; i < g_regionCount && !mine; ++i) {
        NearRegion& r = g_regions[i];
        if (p < r.base || p >= r.base + r.pages * page) continue;
        const unsigned index = static_cast<unsigned>((p - r.base) / page);
        VirtualFree(r.base + index * page, page, MEM_DECOMMIT);
        r.used &= ~(uint64_t(1) << index);
        mine = true;
    }
    ReleaseSRWLockExclusive(&g_poolLock);
    return mine;
}

// A trampoline's memory, committed read-write: near `anchor` (a pool page) when a moved instruction is rip-relative,
// anywhere otherwise.
uint8_t* allocateTrampoline(bool needNear, void* anchor, size_t bytes) {
    if (bytes > pageSize()) return nullptr;
    if (needNear) return poolTake(reinterpret_cast<uintptr_t>(anchor));
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}
void freeTrampoline(uint8_t* p) {
    if (!p) return;
    if (!poolGive(p)) VirtualFree(p, 0, MEM_RELEASE);
}

}  // namespace

void codeHookReserveNear(void* anchor) {
    if (!anchor) return;
    AcquireSRWLockExclusive(&g_poolLock);
    addRegion(reinterpret_cast<uintptr_t>(anchor));
    ReleaseSRWLockExclusive(&g_poolLock);
}

uint8_t* codeHookNearPage(const void* target) {
    if (!target) return nullptr;
    return poolTake(reinterpret_cast<uintptr_t>(target));
}

void codeHookFreeNearPage(void* p) {
    freeTrampoline(static_cast<uint8_t*>(p));
}

CodeHookPoolStats codeHookPoolStats() {
    CodeHookPoolStats out;
    AcquireSRWLockShared(&g_poolLock);
    out.regions = g_regionCount;
    for (unsigned i = 0; i < g_regionCount; ++i) {
        out.pages += g_regions[i].pages;
        for (uint64_t u = g_regions[i].used; u; u &= u - 1) ++out.used;
    }
    ReleaseSRWLockShared(&g_poolLock);
    return out;
}

bool codeHookInPool(const void* p) {
    const uintptr_t page = pageSize();
    bool in = false;
    AcquireSRWLockShared(&g_poolLock);
    for (unsigned i = 0; i < g_regionCount && !in; ++i)
        in = p >= g_regions[i].base && p < g_regions[i].base + g_regions[i].pages * page;
    ReleaseSRWLockShared(&g_poolLock);
    return in;
}

void CodeHook::refusedf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(m_refusal, sizeof(m_refusal), fmt, args);
    va_end(args);
    Log::get().note("%s", m_refusal);
}

bool CodeHook::install(void* target, void* replacement, void** origOut,
                       const char* who, Prepare prepare, void* prepareContext) {
    const char* label = who ? who : "?";
    m_refusal[0] = 0;
    if (m_installed || !target || !replacement) return false;

    uint8_t* const code = static_cast<uint8_t*>(target);

    // The patch is one aligned eight-byte store, so no thread can ever see it
    // half-written -- which is what makes this safe to do to a function other
    // threads are calling right now, without suspending any of them. An
    // unaligned entry point cannot be patched that way, and rather than fall
    // back to a torn write this refuses. Compilers align function entries to
    // sixteen bytes, so refusing costs nothing in practice and removes a whole
    // class of race from the design.
    if (reinterpret_cast<uintptr_t>(code) % 8 != 0) {
        refusedf(
            "CodeHook %s: %p is not eight-byte aligned, so the patch could not "
            "be written as one atomic store. Not hooked -- a torn patch in code "
            "another thread is executing is not a risk worth taking.",
            label, target);
        return false;
    }

    // Decode whole instructions until at least the patch is covered. The
    // stolen bytes have to be a whole number of instructions or the trampoline
    // resumes in the middle of one.
    struct Piece {
        size_t offset = 0;
        size_t length = 0;
        size_t ripDisp = 0;   // offset of the disp32 within the instruction, or 0
    };
    Piece pieces[8];
    size_t pieceCount = 0;
    size_t stolen = 0;
    while (stolen < kCodeHookPatchBytes) {
        if (pieceCount >= 8) {
            refusedf("CodeHook %s: the prologue at %p takes more than eight instructions to cover the patch. Not hooked.", label, target);
            return false;
        }
        size_t len = 0;
        size_t rip = 0;
        const bool read = guarded("CodeHook::install/decode", [&] {
            len = codeInstructionLength(code + stolen, 16 - stolen, &rip);
        });
        if (!read || len == 0) {
            refusedf(
                "CodeHook %s: the instruction at %p+%zu is one this build does "
                "not recognise or will not move (a jump or a call -- a function "
                "beginning with one is a linker thunk or somebody else's hook, "
                "and following it would cut them out). Not hooked, and nothing "
                "was changed.",
                label, target, stolen);
            return false;
        }
        pieces[pieceCount].offset = stolen;
        pieces[pieceCount].length = len;
        pieces[pieceCount].ripDisp = rip;
        ++pieceCount;
        stolen += len;
    }
    if (stolen > sizeof(m_original)) {
        refusedf("CodeHook %s: the %zu bytes to move at %p are more than the %zu the trampoline keeps. Not hooked.", label, stolen, target,
                 sizeof(m_original));
        return false;
    }

    // trampoline = the stolen instructions, then an absolute jump back to the
    // rest of the function. Absolute here, not relative: the trampoline is
    // within reach of the target by construction, but the code AFTER the patch
    // does not have to be within reach of the trampoline once both have been
    // placed by the allocator.
    const size_t trampolineBytes = stolen + 14;
    bool needNear = false;
    for (size_t p = 0; p < pieceCount; ++p) needNear |= pieces[p].ripDisp != 0;
    uint8_t* const tramp = allocateTrampoline(needNear, target, trampolineBytes);
    if (!tramp) {
        refusedf(
            "CodeHook %s: no executable memory could be reserved within reach "
            "of %p for the trampoline of its rip-relative prologue (pool: %u region(s), %u of %u page(s) in use). Not hooked.",
            label, target, codeHookPoolStats().regions, codeHookPoolStats().used, codeHookPoolStats().pages);
        return false;
    }

    bool relocatable = true;
    bool built = guarded("CodeHook::install/build", [&] {
        memcpy(tramp, code, stolen);

        // REWRITE EVERY RIP-RELATIVE DISPLACEMENT. The instruction has moved,
        // and the whole meaning of the operand is "this far from where I am",
        // so copying the bytes unchanged points it at different memory --
        // silently, and at a place with no relation to the bug it causes. The
        // arithmetic is: work out what it addressed where it was, then say the
        // same thing from where it now is.
        for (size_t p = 0; p < pieceCount; ++p) {
            if (!pieces[p].ripDisp) continue;
            const size_t dispAt = pieces[p].offset + pieces[p].ripDisp;
            const size_t endAt = pieces[p].offset + pieces[p].length;
            int32_t was = 0;
            memcpy(&was, code + dispAt, sizeof(was));
            const uintptr_t addressed =
                reinterpret_cast<uintptr_t>(code) + endAt + was;
            const intptr_t now = static_cast<intptr_t>(addressed) -
                                 static_cast<intptr_t>(
                                     reinterpret_cast<uintptr_t>(tramp) + endAt);
            if (now > INT32_MAX || now < INT32_MIN) {
                relocatable = false;
                return;
            }
            const int32_t fixed = static_cast<int32_t>(now);
            memcpy(tramp + dispAt, &fixed, sizeof(fixed));
        }

        uint8_t* const back = tramp + stolen;
        back[0] = 0xFF;                       // jmp qword ptr [rip+0]
        back[1] = 0x25;
        back[2] = back[3] = back[4] = back[5] = 0x00;
        const uint64_t resume =
            reinterpret_cast<uintptr_t>(code) + stolen;
        memcpy(back + 6, &resume, sizeof(resume));
        memcpy(m_original, code, stolen);
    });
    if (!built || !relocatable) {
        if (!relocatable) {
            refusedf(
                "CodeHook %s: the prologue at %p reaches data more than two "
                "gigabytes from where the trampoline could be placed, so the "
                "moved instruction could not be made to address it. Not hooked.",
                label, target);
        }
        if (!built) {
            refusedf("CodeHook %s: the trampoline for the prologue at %p could not be written. Not hooked.", label, target);
        }
        freeTrampoline(tramp);
        return false;
    }

    // The trampoline is never written again once it is live, so it is not
    // left writable once it is live either: flip it to executable-only here,
    // before the target is touched. Nothing below writes into it; uninstall
    // only restores the target's eight bytes and releases the block.
    DWORD trampOldProtect = 0;
    if (!VirtualProtect(tramp, trampolineBytes, PAGE_EXECUTE_READ, &trampOldProtect)) {
        refusedf(
            "CodeHook %s: could not make the trampoline at %p executable (err "
            "%lu). Not hooked.",
            label, tramp, GetLastError());
        freeTrampoline(tramp);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), tramp, trampolineBytes);

    // Build the replacement's eight bytes: E9 rel32, then whatever the original
    // held beyond the patch, so the store leaves the tail of the eight-byte
    // word exactly as it found it.
    const intptr_t displacement =
        reinterpret_cast<intptr_t>(replacement) -
        (reinterpret_cast<intptr_t>(code) + static_cast<intptr_t>(kCodeHookPatchBytes));
    if (displacement > INT32_MAX || displacement < INT32_MIN) {
        refusedf(
            "CodeHook %s: %p is more than two gigabytes from the replacement, "
            "which a five-byte jump cannot reach. Not hooked.",
            label, target);
        freeTrampoline(tramp);
        return false;
    }

    uint64_t word = 0;
    memcpy(&word, code, sizeof(word));
    uint8_t patched[8];
    memcpy(patched, &word, sizeof(patched));
    patched[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(displacement);
    memcpy(patched + 1, &rel, sizeof(rel));
    uint64_t newWord = 0;
    memcpy(&newWord, patched, sizeof(newWord));

    // A newly intercepted call can arrive as soon as the entry store below.
    // Relay users must therefore publish their original-function forward now,
    // rather than waiting for origOut after the target has already changed.
    bool prepared = true;
    if (prepare && (!guarded("CodeHook::install/prepare", [&] {
            prepared = prepare(tramp, prepareContext);
        }) || !prepared)) {
        refusedf("CodeHook %s: replacement preparation refused; target unchanged.", label);
        freeTrampoline(tramp);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(code, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        refusedf("CodeHook %s: could not make %p writable (err %lu). Not "
                        "hooked.", label, target, GetLastError());
        freeTrampoline(tramp);
        return false;
    }
    // ONE aligned store. Every other thread sees either the old eight bytes or
    // the new ones, never a mixture, so none of them has to be stopped first.
    const bool wrote = guarded("CodeHook::install/patch", [&] {
        *reinterpret_cast<volatile uint64_t*>(code) = newWord;
    });
    DWORD ignored = 0;
    VirtualProtect(code, 8, oldProtect, &ignored);
    if (!wrote) {
        freeTrampoline(tramp);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), code, 8);

    m_target = target;
    m_replacement = replacement;
    m_trampoline = tramp;
    m_stolen = stolen;
    m_installed = true;
    if (origOut) *origOut = tramp;

    Log::get().note(
        "CodeHook %s: hooked the function at %p itself, moving %zu byte(s) of "
        "its prologue to a trampoline at %p (%s). Whatever is written into the "
        "vtable slot that names this function, calls to it now arrive here -- "
        "which is the point: the table can be restored as often as its owner "
        "likes and there is nothing left to lose a race with.",
        label, target, stolen, static_cast<void*>(tramp),
        needNear ? "near: a page of the pool, the prologue is rip-relative" : "anywhere: no rip-relative instruction moved");
    return true;
}

void CodeHook::uninstall() {
    if (!m_installed) return;
    uint8_t* const code = static_cast<uint8_t*>(m_target);

    // Only if the patch is still ours. Another tool that hooked this function
    // after EDVR owns those bytes now; putting the originals back would splice
    // it out, which is precisely the thing done to EDVR that this class was
    // built to survive.
    bool ours = false;
    guarded("CodeHook::uninstall/check", [&] {
        const intptr_t displacement =
            reinterpret_cast<intptr_t>(m_replacement) -
            (reinterpret_cast<intptr_t>(code) +
             static_cast<intptr_t>(kCodeHookPatchBytes));
        int32_t rel = 0;
        memcpy(&rel, code + 1, sizeof(rel));
        ours = code[0] == 0xE9 && rel == static_cast<int32_t>(displacement);
    });

    if (ours) {
        uint64_t word = 0;
        guarded("CodeHook::uninstall/read", [&] {
            memcpy(&word, code, sizeof(word));
        });
        uint8_t bytes[8];
        memcpy(bytes, &word, sizeof(bytes));
        memcpy(bytes, m_original, m_stolen < 8 ? m_stolen : 8);
        uint64_t restored = 0;
        memcpy(&restored, bytes, sizeof(restored));

        DWORD oldProtect = 0;
        if (VirtualProtect(code, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            guarded("CodeHook::uninstall/patch", [&] {
                *reinterpret_cast<volatile uint64_t*>(code) = restored;
            });
            DWORD ignored = 0;
            VirtualProtect(code, 8, oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), code, 8);
        }
    } else {
        Log::get().note(
            "CodeHook: the function at %p was hooked again after EDVR, so its "
            "first bytes belong to that tool now and are being left alone. "
            "EDVR's trampoline is deliberately leaked rather than freed -- "
            "their hook still runs through it, and freeing it would dangle.",
            m_target);
    }

    // Freed only when the patch was still ours; see above.
    if (ours && m_trampoline) freeTrampoline(m_trampoline);
    m_trampoline = nullptr;
    m_target = nullptr;
    m_replacement = nullptr;
    m_stolen = 0;
    m_installed = false;
}

}  // namespace edvr
