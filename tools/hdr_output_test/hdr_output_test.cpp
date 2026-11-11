// hdr_output_test -- fix.hdr's two decisions (src\d3d11\hdr_output_rules.h): which of Elite's textures are created wide, and what
// format a view over a widened one gets. Both are invisible when wrong -- a predicate that misses leaves the HDR clamped to SDR while
// every log line still says the feature ran -- so they are held here to the cases that matter: the real tone target at its real size,
// the oracle's two bind classes, the exclusions, and a view that cannot be mapped exactly. Built from the repo root by build.bat's
// :rig_hdr_output_test. Windows-free: it includes only the pure header. --dry-run writes nothing; --self-test runs the checks.
#include "../../src/d3d11/hdr_output_rules.h"
#include "../../src/d3d11/hdr_shader_identity.h"
#ifdef _WIN32
#include "hdr_shader_tests.h"
#endif

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

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
    for (const char* key : {"\"advanced.hdr_scene_nits\"", "\"advanced.hdr_ui_nits\"",
                            "\"advanced.hdr_peak_nits\"", "\"advanced.hdr_gamma\"",
                            "\"advanced.hdr_highlights\"", "\"advanced.hdr_widen\"",
                            "\"advanced.hdr_grading\""})
        check(text.find(key) != std::string::npos,
              "the flat profile names each HDR brightness and gamma key, so a flat file's value is read");
}

// The brightness the shaders are built with. Unset keys keep the fitted
// defaults; the peak follows the display unless configured; nothing can make
// the shoulder's range empty.
void theBrightnessRule() {
    const auto unset = rules::resolveNits(0, 0, 0, 1600);
    check(unset.scene == 261.0f && unset.ui == 500.0f,
          "unset scene and UI white keep 261 and 500 nits");
    check(unset.peak == 1600.0f && unset.peakFromDisplay,
          "an unset peak is the display's MaxLuminance (MacBook Pro XDR: 1600)");
    check(rules::resolveNits(0, 0, 0, 2000).peak == 2000.0f,
          "a brighter panel's report is used as it is");
    const auto set = rules::resolveNits(400, 350, 1200, 1600);
    check(set.scene == 400.0f && set.ui == 350.0f && set.peak == 1200.0f && !set.peakFromDisplay,
          "configured values win over the defaults and the display");
    const auto noDisplay = rules::resolveNits(0, 0, 0, 0);
    check(noDisplay.peak == rules::kFallbackPeakNits && !noDisplay.peakFromDisplay,
          "no display report falls back to 1000 nits");
    check(rules::resolveNits(0, 0, 0, 100).peak == rules::kFallbackPeakNits,
          "an implausible display report (an SDR panel's 100) is not a peak");
    const auto low = rules::resolveNits(1000, 0, 600, 0);
    check(low.peak == 1000.0f * rules::kMinPeakOverScene,
          "a peak below the scene white is raised to keep the shoulder's range");
    const auto clamped = rules::resolveNits(5, 99999, 99999, 0);
    check(clamped.scene == rules::kMinWhiteNits && clamped.ui == rules::kMaxWhiteNits &&
              clamped.peak == rules::kMaxPeakNits,
          "out-of-range values clamp to the documented bounds");
}

// advanced.hdr_widen: "all" or a candidate list. A list that parses to
// nothing widens nothing, which is why the flat gate must name the key.
void theWidenList() {
    unsigned long long mask = 1;
    check(rules::parseWidenList("all", &mask) && mask == 0, "all widens everything");
    check(rules::parseWidenList("", &mask), "empty is all");
    check(rules::parseWidenList(nullptr, &mask), "unset is all");
    check(!rules::parseWidenList("1,3, 8", &mask) && mask == 0x85ull, "a list sets one bit per candidate");
    check(!rules::parseWidenList("0,65,x,2", &mask) && mask == 0x2ull, "out-of-range and non-numbers are ignored");
    check(rules::widenSelected(false, 0x85ull, 3) && !rules::widenSelected(false, 0x85ull, 2),
          "selection follows the mask");
    check(rules::widenSelected(true, 0, 40), "all selects any candidate");
    check(!rules::parseWidenList("off", &mask) && mask == 0,
          "the flat gate's \"off\" would widen nothing (so the key must be named)");
}

// MSVC refuses a string literal over 16380 bytes (C2026), and clang does not,
// so a shader that grows past it builds on the Mac and fails only in Windows CI.
// Every raw-literal piece of the HDR shader header must stay under the limit.
void theShaderLiteralsFitMsvc() {
    std::FILE* f = std::fopen("src/d3d11/hdr_output_shaders.h", "rb");
    check(f != nullptr, "the HDR shader header can be read from the repo root");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    size_t pieces = 0, largest = 0, pos = 0;
    const std::string open = "R\"HLSL(", close = ")HLSL\"";
    while ((pos = text.find(open, pos)) != std::string::npos) {
        const size_t start = pos + open.size();
        const size_t end = text.find(close, start);
        if (end == std::string::npos) break;
        ++pieces;
        if (end - start > largest) largest = end - start;
        pos = end + close.size();
    }
    check(pieces >= 5 && largest < 16000,
          "every HDR shader string-literal piece is under MSVC's 16380-byte limit");
}

// advanced.hdr_widen = auto: the tone target recognised by its learned shape.
void theLearnedToneTarget() {
    unsigned long long mask = 1;
    check(rules::parseWidenMode(nullptr, &mask) == rules::WidenMode::Auto &&
              rules::parseWidenMode("", &mask) == rules::WidenMode::Auto &&
              rules::parseWidenMode("AUTO", &mask) == rules::WidenMode::Auto,
          "unset, empty and auto are the learned mode");
    check(rules::parseWidenMode(" all", &mask) == rules::WidenMode::All, "all widens every candidate");
    check(rules::parseWidenMode("27, 3", &mask) == rules::WidenMode::List && mask == ((1ull << 26) | 4ull),
          "a number list is the diagnostic list");
    check(rules::parseWidenMode("off", &mask) == rules::WidenMode::List && mask == 0,
          "the flat gate's \"off\" is an empty list that would widen nothing (so the key must be named)");

    rules::TextureShape tone;
    check(rules::parseShapeLine("tone 2268 1473 40 1 0 1 1\n", &tone) && tone.width == 2268 &&
              tone.height == 1473 && tone.bindFlags == 40 && tone.mipLevels == 1,
          "a saved line reads back as the census's tone target (2268x1473, RT|SRV)");
    rules::TextureShape junk;
    check(!rules::parseShapeLine("# comment", &junk) && !rules::parseShapeLine("tone 1 2 3", &junk) &&
              !rules::parseShapeLine("tone 0 1473 40 1 0 1 1", &junk) &&
              !rules::parseShapeLine("tone 99999999999 1 40 1 0 1 1", &junk),
          "comments, short lines, zero sizes and overflow are not shapes");

    rules::TextureShape shapes[rules::kMaxToneShapes];
    unsigned count = 0;
    check(rules::widenInAuto(shapes, count, tone, false), "with nothing learned every candidate widens");
    check(rules::rememberShape(shapes, &count, tone) && count == 1, "the first tone target is learned");
    check(!rules::rememberShape(shapes, &count, tone) && count == 1, "learning it again changes nothing");
    rules::TextureShape uav = tone; uav.bindFlags = 0xA8;
    rules::TextureShape panel{612, 248, 40, 1, 0, 1, 1};
    check(rules::widenInAuto(shapes, count, tone, false) && !rules::widenInAuto(shapes, count, uav, false) &&
              !rules::widenInAuto(shapes, count, panel, false),
          "once learned, only that shape widens: not the same-size UAV target, not a UI panel");
    check(rules::widenInAuto(shapes, count, panel, true), "a miss this session widens everything again");
    rules::TextureShape other = tone; other.width = 3024; other.height = 1964;
    check(rules::rememberShape(shapes, &count, other) && count == 2 && shapes[0] == other && shapes[1] == tone,
          "a new shape goes first and the old one is kept");
    check(rules::rememberShape(shapes, &count, tone) && count == 2 && shapes[0] == tone && shapes[1] == other,
          "a known shape seen again moves to the front without duplicating");
    for (unsigned i = 0; i < 20; ++i) { rules::TextureShape s{100 + i, 100, 40, 1, 0, 1, 1}; rules::rememberShape(shapes, &count, s); }
    check(count == rules::kMaxToneShapes && shapes[0].width == 119,
          "the list is bounded, newest first");
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

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    bool hardware = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) {
            std::printf("hdr_output_test: would run the widening predicate and the view-format "
                        "rule over the tone target, the exclusions and the inexact views, and read "
                        "src/common/flat_profile_keys.h to check the HDR keys are gated in, check the "
                        "brightness rule, and run the "
                        "display-mode retry rule and HDR shader identities. On Windows, also "
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
    theBrightnessRule();
    theWidenList();
    theShaderLiteralsFitMsvc();
    theLearnedToneTarget();
    shaderIdentities();
#ifdef _WIN32
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
                "unwidened targets are left alone; the flat profile names fix.hdr, "
                "advanced.hdr_encode and the brightness keys; scene/UI/peak nits resolve with the "
                "display's peak as the default; the display-mode retry fires only on no-such-FP16-mode "
                "inside our own transaction; HDR-local identities match exactly; on Windows "
                "tone resource contracts and exposure execution also pass)\n", g_checks);
    return 0;
}
