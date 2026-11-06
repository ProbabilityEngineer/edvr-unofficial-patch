// hdr_output_test -- fix.hdr's two decisions (src\d3d11\hdr_output_rules.h): which of Elite's textures are created wide, and what
// format a view over a widened one gets. Both are invisible when wrong -- a predicate that misses leaves the HDR clamped to SDR while
// every log line still says the feature ran -- so they are held here to the cases that matter: the real tone target at its real size,
// the oracle's two bind classes, the exclusions, and a view that cannot be mapped exactly. Built from the repo root by build.bat's
// :rig_hdr_output_test. Windows-free: it includes only the pure header. --dry-run writes nothing; --self-test runs the checks.
#include "../../src/d3d11/hdr_output_rules.h"
#include "../../src/d3d11/hdr_shader_identity.h"
#include "../../src/d3d11/present_dispatch.h"
#include "texture_lifetime_tests.h"
#ifdef _WIN32
#include "hdr_shader_tests.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

#ifdef _WIN32
// The production capture is tested with breadcrumbs disabled. If that gate
// unexpectedly opens, fail rather than write into a live game's log location.
namespace edvr {
void breadcrumb(const char*) {
    std::fputs("FAIL: unexpected breadcrumb during texture lifetime test\n", stderr);
    std::abort();
}
}
#endif

namespace {

namespace rules = edvr::hdrrules;

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

// Elite's post-tonemap render target as EDHDR observed it: RGBA8-typeless,
// render-target bound, 2160x1396 -- a size nothing in the rule may look at.
rules::TextureAsk toneTarget() {
    rules::TextureAsk ask;
    ask.format = rules::kRgba8Typeless;
    ask.bindFlags = rules::kBindRenderTarget | rules::kBindShaderResource;
    return ask;
}

void theToneTargetWidens() {
    check(rules::widens(toneTarget()), "the tone target widens");
    // The defect this rule replaced: eligibility used to require the
    // backbuffer's size, and the tone target never has it. Size is not a
    // field of TextureAsk at all, which is the structural form of that fix,
    // so the check here is that the bind class alone decides.
    rules::TextureAsk uav = toneTarget();
    uav.bindFlags = rules::kBindUnorderedAccess;
    check(rules::widens(uav), "the unordered-access class widens too");
    rules::TextureAsk both = toneTarget();
    both.bindFlags = rules::kBindRenderTarget | rules::kBindUnorderedAccess;
    check(rules::widens(both), "a target in both classes widens");
}

void theWrongThingsDoNot() {
    rules::TextureAsk sampled = toneTarget();
    sampled.bindFlags = rules::kBindShaderResource;
    check(!rules::widens(sampled), "a sampled-only texture does not widen");

    rules::TextureAsk typed = toneTarget();
    typed.format = rules::kRgba8Unorm;
    check(!rules::widens(typed), "an already-typed RGBA8 target does not widen");
    typed.format = rules::kBgra8Unorm;
    check(!rules::widens(typed), "a BGRA8 target does not widen");
    typed.format = rules::kRgba16Float;
    check(!rules::widens(typed), "a target that is already wide does not widen");

    rules::TextureAsk multisample = toneTarget();
    multisample.sampleCount = 4;
    check(!rules::widens(multisample), "a multisample target does not widen");

    rules::TextureAsk array = toneTarget();
    array.arraySize = 6;
    check(!rules::widens(array), "an array target does not widen");

    for (unsigned misc : {rules::kMiscShared, rules::kMiscSharedKeyedMutex,
                          rules::kMiscSharedNtHandle}) {
        rules::TextureAsk shared = toneTarget();
        shared.miscFlags = misc;
        check(!rules::widens(shared), "a shared target does not widen");
    }

    rules::TextureAsk staging = toneTarget();
    staging.usage = rules::kUsageStaging;
    check(!rules::widens(staging), "a staging target does not widen");

    rules::TextureAsk mapped = toneTarget();
    mapped.cpuAccessFlags = 0x10000;   // D3D11_CPU_ACCESS_WRITE
    check(!rules::widens(mapped), "a CPU-visible target does not widen");
}

void viewsOverAWidenedTarget() {
    unsigned mapped = 0;
    bool exact = false;

    check(rules::viewFormat(rules::kRgba16Typeless, rules::kRgba8Unorm, &mapped, &exact) &&
              mapped == rules::kRgba16Float && exact,
          "a UNORM view over a widened target becomes FP16, exactly");
    check(rules::viewFormat(rules::kRgba16Typeless, rules::kRgba8UnormSrgb, &mapped, &exact) &&
              mapped == rules::kRgba16Float && exact,
          "an sRGB view over a widened target becomes FP16, exactly");

    // Elite's 8-bit render-target view over the FP16 backbuffer -- the format
    // mismatch that used to remove the device. The backbuffer is FLOAT, not
    // TYPELESS, so the rule has to cover both.
    check(rules::viewFormat(rules::kRgba16Float, rules::kRgba8Unorm, &mapped, &exact) &&
              mapped == rules::kRgba16Float && exact,
          "an 8-bit view over the FP16 backbuffer becomes FP16");
    check(rules::viewFormat(rules::kRgba16Float, rules::kBgra8Unorm, &mapped, &exact) &&
              mapped == rules::kRgba16Float && exact,
          "a BGRA view over the FP16 backbuffer becomes FP16");

    // Mapped, because refusing a view create is fatal in Elite -- but NOT
    // exact, and the caller logs that, because EDHDR's clone chain could have
    // served this one from the 8-bit original.
    for (unsigned format : {rules::kRgba8Uint, rules::kRgba8Snorm, rules::kRgba8Sint}) {
        exact = true;
        check(rules::viewFormat(rules::kRgba16Typeless, format, &mapped, &exact) &&
                  mapped == rules::kRgba16Float && !exact,
              "a reinterpreting view is mapped but reported inexact");
    }
}

void viewsThatAreNoneOfOurBusiness() {
    unsigned mapped = 0xFFFFFFFFu;
    bool exact = false;
    check(!rules::viewFormat(rules::kRgba8Typeless, rules::kRgba8Unorm, &mapped, &exact),
          "a view over an unwidened 8-bit target is left alone");
    check(!rules::viewFormat(rules::kRgba16Typeless, rules::kRgba16Float, &mapped, &exact),
          "an FP16 view over a widened target is left alone");
    check(!rules::viewFormat(rules::kRgba16Typeless, rules::kUnknown, &mapped, &exact),
          "a default-format view is left alone");
    check(mapped == 0xFFFFFFFFu, "a rule that declines writes nothing");
}

// The two helpers the caller orders its tests by: the cheap format test runs
// before the resource is asked what it is, so it has to agree with the rule.
void theCheapTestAgreesWithTheRule() {
    const unsigned eight[] = {rules::kRgba8Typeless, rules::kRgba8Unorm, rules::kRgba8UnormSrgb,
                              rules::kRgba8Uint, rules::kRgba8Snorm, rules::kRgba8Sint,
                              rules::kBgra8Typeless, rules::kBgra8Unorm,
                              rules::kBgra8UnormSrgb, rules::kBgrx8Unorm};
    bool allAgree = true;
    for (unsigned format : eight) {
        unsigned mapped = 0;
        bool exact = false;
        if (!rules::eightBitRgba(format) ||
            !rules::viewFormat(rules::kRgba16Typeless, format, &mapped, &exact))
            allAgree = false;
    }
    check(allAgree, "every 8-bit format the cheap test admits is one the rule maps");
    check(!rules::eightBitRgba(rules::kRgba16Float) && !rules::eightBitRgba(rules::kUnknown),
          "the cheap test declines the wide and default formats");
    check(rules::exactlyCompatible(rules::kRgba8Unorm) &&
              !rules::exactlyCompatible(rules::kRgba8Uint),
          "exact compatibility is the UNORM forms and not the reinterpretations");
}

// The display-mode retry (hdr_fullscreen_mode.cpp). An FP16 swapchain has no
// FP16 display mode, so the game's fullscreen path is refused as often as it
// asks -- 940 times on the 2026-11-01 flight. Asking the same mode again as
// 8-bit is the fix, and being too eager with it is the quiet danger: a retry
// over a native validation failure would hide a real refusal behind a second
// call with different arguments.
void theDisplayModeRetry() {
    const unsigned notFound = rules::kErrorNotFound;
    check(rules::retriesModeInEightBit(notFound, true, rules::kRgba16Float, true, true, false),
          "the observed failure -- no FP16 display mode, in our own transaction -- is retried");
    check(!rules::retriesModeInEightBit(0, true, rules::kRgba16Float, true, true, false),
          "a mode that was found is never asked for twice");
    // 0x887A0001 is DXGI_ERROR_INVALID_CALL -- exactly the kind of native
    // validation failure a retry must never paper over.
    check(!rules::retriesModeInEightBit(0x887A0001u, true, rules::kRgba16Float, true, true, false),
          "a different DXGI failure is left exactly as the runtime gave it");
    check(!rules::retriesModeInEightBit(notFound, false, rules::kRgba16Float, true, true, false),
          "a chain outside our own transaction is not touched");
    check(!rules::retriesModeInEightBit(notFound, true, rules::kRgba8Unorm, true, true, false),
          "an 8-bit request that failed is the display's own answer, not ours to rewrite");
    check(!rules::retriesModeInEightBit(notFound, true, rules::kRgba16Float, false, true, false),
          "no request to copy, no retry");
    check(!rules::retriesModeInEightBit(notFound, true, rules::kRgba16Float, true, false, false),
          "nowhere to put the answer, no retry");
    check(!rules::retriesModeInEightBit(notFound, true, rules::kRgba16Float, true, true, true),
          "a request carrying a device filter is a different question; it is forwarded");
}

// The gate that cost the 2026-11-01 flight. In the flat profile, Config's
// getString answers "off" for any key src\common\flat_profile_keys.h does not
// name -- before it looks at the file. fix.hdr was not named, so a file saying
// "hdr = on" read as off, hdrOutputStartup returned at its first branch, and
// the session wrote NOT ONE "hdr:" line: no banner, no refusal, nothing to
// read. The feature was whole and simply never asked to run.
//
// Nothing in C++ can be asserted about that gate from here -- the header is
// Windows-free on purpose and the gate is not -- so the check is on the text
// of the file itself, which is what the other flat features' rigs do
// (sharp_weapons_test, aa_compare_test). Run from the repo root by build.bat.
// A file it cannot open is a FAIL and not a skip: a check that quietly skips
// is the same kind of defect it is here to catch.
void theFlatProfileReadsTheHdrKeys() {
    std::FILE* f = std::fopen("src/common/flat_profile_keys.h", "rb");
    check(f != nullptr, "the flat profile's key list can be read from the repo root");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    check(text.find("\"fix.hdr\"") != std::string::npos,
          "the flat profile names fix.hdr, so a flat file's hdr = on is actually read");
    check(text.find("\"advanced.hdr_encode\"") != std::string::npos,
          "the flat profile names advanced.hdr_encode, so the domain A/B can be set");
}

void shaderIdentities() {
    using namespace edvr;
    const auto hello = hdrShaderIdentity("hello", 5);
    check(hello.fnv == 0xa430d84680aabd0bull && hello.crc == 0x3610a686u,
          "HDR uses standard FNV-1a and reflected CRC32, not EDVR's legacy offset");
    const HdrShaderIdentity ids[] = {
        {3192, 0xD7E646E3u, 0x75028D69C8E1B8E9ull},
        {2244, 0x6BE69EF3u, 0x3B86E25519AA3DA6ull},
        {268, 0xD6A846C8u, 0x3C4D865B7031B44Cull}};
    const HdrShaderKind kinds[] = {HdrShaderKind::ToneBloom, HdrShaderKind::ToneNoBloom, HdrShaderKind::Copy};
    for (unsigned i = 0; i < 3; ++i) {
        check(hdrShaderMatch(ids[i]) == kinds[i], "exact identity chooses its own HDR variant");
        auto bad = ids[i]; ++bad.bytes;
        check(hdrShaderMatch(bad) == HdrShaderKind::None, "wrong length refuses");
        bad = ids[i]; bad.crc ^= 1;
        check(hdrShaderMatch(bad) == HdrShaderKind::None, "wrong CRC refuses");
        bad = ids[i]; bad.fnv ^= 1;
        check(hdrShaderMatch(bad) == HdrShaderKind::None, "wrong FNV refuses");
    }
    check(hdrShaderKind(nullptr, 268) == HdrShaderKind::None, "null bytecode refuses");
    check(hdrShaderKind("hello", 5) == HdrShaderKind::None, "unrelated shader refuses");
}

void presentStatusOnly() {
    using namespace edvr;
    int owned = 0, foreign = 0;
    bool routesCorrect = true, forwardsExact = true, observesExact = true;
    // Include TEST combined with another flag, a non-TEST flag, occlusion,
    // retry and failure HRESULTs. None may alter the forwarding contract.
    for (bool ours : {false, true}) {
        for (uint32_t flags : {0u, 1u, 9u, 8u}) {
            for (uint32_t result : {0u, kPresentOccluded, 0x887a000au, 0x80004005u}) {
                int* chain = ours ? &owned : &foreign;
                unsigned forwards = 0, frameWork = 0, observations = 0;
                unsigned gpuWork = 0, frameState = 0;
                auto forward = [&](int* got, uint32_t interval, uint32_t gotFlags) {
                    ++forwards;
                    forwardsExact &= got == chain && interval == 3 && gotFlags == flags;
                    return static_cast<int32_t>(result);
                };
                auto frame = [&](int* got, uint32_t interval, uint32_t gotFlags) {
                    ++frameWork; ++gpuWork; ++frameState;
                    return forward(got, interval, gotFlags);
                };
                auto observe = [&](uint32_t gotFlags, int32_t gotResult) {
                    ++observations;
                    observesExact &= forwards == 1 && gotFlags == flags &&
                                     static_cast<uint32_t>(gotResult) == result;
                };
                const auto actual = dispatchPresent(chain, &owned, 3, flags, forward, frame, observe);
                const unsigned expectedWork = ours && !(flags & kPresentTestFlag) ? 1u : 0u;
                routesCorrect &= forwards == 1 && frameWork == expectedWork &&
                                 gpuWork == expectedWork && frameState == expectedWork &&
                                 observations == (ours ? 1u : 0u);
                forwardsExact &= static_cast<uint32_t>(actual) == result;
            }
        }
    }
    check(routesCorrect, "TEST and foreign Presents forward once without GPU/frame work; normal owned Presents retain it");
    check(forwardsExact, "Present preserves chain, interval, all flags and exact HRESULT, including failures");
    check(observesExact, "owned Present status is observed only after forwarding");

    PresentStatusCounters counts;
    PresentStatusSnapshot snapshot;
    check(counts.note(0, 0, 0, snapshot) && snapshot.regular == 1 && snapshot.tests == 0,
          "initial status report includes zero TEST calls rather than silent instrumentation");
    bool quiet = !counts.note(1, 0, 1, snapshot);
    quiet &= !counts.note(9, kPresentOccluded, 2, snapshot);
    quiet &= !counts.note(0, kPresentOccluded, 3, snapshot);
    quiet &= !counts.note(1, 0x80004005u, 4, snapshot);
    check(quiet, "status reports are rate limited while counters continue accumulating");
    check(counts.note(0, 0, PresentStatusCounters::kReportMs, snapshot) &&
              snapshot.tests == 3 && snapshot.regular == 3 &&
              snapshot.testOccluded == 1 && snapshot.regularOccluded == 1,
          "TEST and real-present occlusion are counted separately, including intervening calls");
    bool bounded = true;
    for (unsigned i = 2; i < PresentStatusCounters::kMaxReports; ++i)
        bounded &= counts.note(0, 0, i * PresentStatusCounters::kReportMs, snapshot);
    bounded &= snapshot.report == PresentStatusCounters::kMaxReports;
    bounded &= !counts.note(1, kPresentOccluded,
                           PresentStatusCounters::kMaxReports * PresentStatusCounters::kReportMs, snapshot);
    check(bounded, "status output stops at the session cap with a final-report marker available");
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    bool hardware = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) {
            std::printf("hdr_output_test: would run the widening predicate and the view-format "
                        "rule over the tone target, the exclusions and the inexact views, and read "
                        "src/common/flat_profile_keys.h to check fix.hdr is gated in, and run the "
                        "display-mode retry rule, HDR shader identities, TEST-Present dispatch/counters "
                        "and bounded texture-lifetime accounting. On Windows, also "
                        "check compressed-texture lifetimes through production state capture, "
                        "compile/reflect tone variants and execute exposure on WARP "
                        "(--hardware-self-test selects the hardware adapter). "
                        "Writes nothing.\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
        if (std::strcmp(argv[i], "--hardware-self-test") == 0) { selfTest = true; hardware = true; }
    }
    if (!selfTest) {
        std::printf("usage: hdr_output_test [--dry-run] [--self-test] [--hardware-self-test]\n");
        return 0;
    }
    theToneTargetWidens();
    theWrongThingsDoNot();
    viewsOverAWidenedTarget();
    viewsThatAreNoneOfOurBusiness();
    theCheapTestAgreesWithTheRule();
    theDisplayModeRetry();
    theFlatProfileReadsTheHdrKeys();
    shaderIdentities();
    presentStatusOnly();
    texture_lifetime_tests::run(check);
#ifdef _WIN32
    texture_lifetime_tests::runCom(check);
    hdr_shader_tests::run(check, hardware);
#else
    (void)hardware;
#endif
    if (g_fails) {
        std::printf("FAIL: hdr_output_test %d of %d checks failed\n", g_fails, g_checks);
        return 1;
    }
    std::printf("PASS: hdr_output_test %d checks (the tone target and both bind classes widen with no "
                "size test; typed, multisample, array, shared, staging and CPU-visible targets do not; "
                "UNORM and sRGB views map exactly, UINT/SNORM/SINT map inexactly and say so; views over "
                "unwidened targets are left alone; the flat profile names fix.hdr and "
                "advanced.hdr_encode; the display-mode retry fires only on no-such-FP16-mode "
                "inside our own transaction; HDR-local identities match exactly; TEST-Present "
                "dispatch preserves forwarding without frame work and status output is bounded; "
                "texture lifetime accounting is bounded and balanced; on Windows "
                "tone resource contracts and exposure execution also pass)\n", g_checks);
    return 0;
}
