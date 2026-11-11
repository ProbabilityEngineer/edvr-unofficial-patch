// fix.hdr's two decisions, as arithmetic.
//
// Which of the game's textures are created wide, and what format a view over a
// widened one gets, are the two places where being subtly wrong is invisible:
// a predicate that misses leaves the HDR clamped to SDR while every other log
// line still says the feature ran, and a view mapped wrongly shows up as a
// wrong-looking pass rather than as an error. Both are pure functions of a
// description, so they live here, in plain integers with no Windows headers,
// and a test can run them anywhere -- including on the machine the DLL is
// cross-built from.
//
// hdr_output.cpp holds the D3D11 side and does nothing with a desc that these
// functions do not decide.
#pragma once

namespace edvr {
namespace hdrrules {

// The DXGI format codes this feature cares about, by value, because a rule
// written in plain integers is a rule a test can reach without d3d11.h.
// Checked against dxgiformat.h.
// Plain unsigned, not an enum: hdr_output.cpp holds each of these to the real
// DXGI or D3D11 enumerator with a static_assert, and comparing two different
// enum types to do that is a warning in its own right.
constexpr unsigned kUnknown = 0;
constexpr unsigned kRgba16Typeless = 9;
constexpr unsigned kRgba16Float = 10;
constexpr unsigned kRgba8Typeless = 27;
constexpr unsigned kRgba8Unorm = 28;
constexpr unsigned kRgba8UnormSrgb = 29;
constexpr unsigned kRgba8Uint = 30;
constexpr unsigned kRgba8Snorm = 31;
constexpr unsigned kRgba8Sint = 32;
constexpr unsigned kBgra8Unorm = 87;
constexpr unsigned kBgrx8Unorm = 88;
constexpr unsigned kBgra8Typeless = 90;
constexpr unsigned kBgra8UnormSrgb = 91;

// D3D11_BIND_*, D3D11_RESOURCE_MISC_SHARED* and D3D11_USAGE_*, likewise by value.
constexpr unsigned kBindShaderResource = 0x8;
constexpr unsigned kBindRenderTarget = 0x20;
constexpr unsigned kBindUnorderedAccess = 0x80;
constexpr unsigned kMiscShared = 0x2;
constexpr unsigned kMiscSharedKeyedMutex = 0x100;
constexpr unsigned kMiscSharedNtHandle = 0x800;
constexpr unsigned kUsageDefault = 0;
constexpr unsigned kUsageStaging = 3;

// What a CreateTexture2D asks for, reduced to the fields the rule reads.
struct TextureAsk {
    unsigned format = kUnknown;
    unsigned bindFlags = 0;
    unsigned miscFlags = 0;
    unsigned usage = kUsageDefault;
    unsigned cpuAccessFlags = 0;
    unsigned sampleCount = 1;
    unsigned arraySize = 1;
};

// True when that texture is created R16G16B16A16_TYPELESS instead.
//
// The rule is the oracle's (RenoDX's Elite preset): an RGBA8-TYPELESS resource
// in the render-target class or the unordered-access class, upgraded with
// ignore_size. Format and bind flags only -- NO size test of any kind. Elite's
// real tone target is 2160x1396 against a 2336x1510 display, and an
// equality-with-the-backbuffer rule is exactly what used to miss it.
//
// The exclusions are this port's, not the oracle's, and each one is a case
// where widening would change a contract rather than a precision:
//   * multisample or array resources, which the post chain does not use and
//     whose wide forms are not covered by anything that has been tried;
//   * shared resources, whose format is part of the agreement with whoever
//     opens the handle;
//   * staging and CPU-visible resources, because somebody computes a row
//     pitch for those and widening doubles it.
inline bool widens(const TextureAsk& ask) {
    if (ask.format != kRgba8Typeless) return false;
    if (!(ask.bindFlags & (kBindRenderTarget | kBindUnorderedAccess))) return false;
    if (ask.sampleCount != 1 || ask.arraySize != 1) return false;
    if (ask.miscFlags & (kMiscShared | kMiscSharedKeyedMutex | kMiscSharedNtHandle))
        return false;
    if (ask.usage == kUsageStaging || ask.cpuAccessFlags != 0) return false;
    return true;
}

// An 8-bit RGBA or BGRA view format: the kind the game asks for over a target
// this feature widened, and over the FP16 backbuffer.
inline bool eightBitRgba(unsigned format) {
    switch (format) {
        case kRgba8Typeless: case kRgba8Unorm: case kRgba8UnormSrgb:
        case kRgba8Uint: case kRgba8Snorm: case kRgba8Sint:
        case kBgra8Typeless: case kBgra8Unorm: case kBgra8UnormSrgb: case kBgrx8Unorm:
            return true;
        default:
            return false;
    }
}

// Colour-compatible with R16G16B16A16_FLOAT: the UNORM forms, whose values
// mean the same thing in a wider container. A UINT, SNORM or SINT view is a
// reinterpretation of the bits and is NOT compatible.
inline bool exactlyCompatible(unsigned format) {
    switch (format) {
        case kRgba8Unorm: case kRgba8UnormSrgb:
        case kBgra8Unorm: case kBgra8UnormSrgb: case kBgrx8Unorm:
            return true;
        default:
            return false;
    }
}

inline bool wideResource(unsigned format) {
    return format == kRgba16Typeless || format == kRgba16Float;
}

// The format a view gets. `mapped` is written and true returned only when the
// view has to be rebuilt; false means pass the game's desc through untouched.
//
// `exact` says whether the mapping preserves what the view meant. False is not
// an error and must not refuse the create -- a refused create is fatal in
// Elite -- but it is worth a line in the log, because EDHDR's clone chain
// could serve such a view from the 8-bit original and this cannot.
inline bool viewFormat(unsigned resourceFormat, unsigned askedFormat, unsigned* mapped,
                       bool* exact) {
    if (!eightBitRgba(askedFormat)) return false;
    if (!wideResource(resourceFormat)) return false;
    if (mapped) *mapped = kRgba16Float;
    if (exact) *exact = exactlyCompatible(askedFormat);
    return true;
}

// ---- the display-mode retry ---------------------------------------------

// DXGI_ERROR_NOT_FOUND, by value, for the same reason the formats are.
constexpr unsigned kErrorNotFound = 0x887A0002u;

// Whether a failed FindClosestMatchingMode1 should be asked again in 8-bit.
//
// An FP16 swapchain has no FP16 DISPLAY mode, so the game's fullscreen path
// asks for one that cannot exist and is told so, over and over (940 times on
// the 2026-11-01 flight). The answer is to ask the same mode as
// R8G8B8A8_UNORM, which is the format the display actually scans out.
//
// Being too eager here is the quiet danger: a retry on a native validation
// failure would hide a real refusal behind a second call with different
// arguments, and a retry outside our own transaction would reach a chain this
// feature never upgraded. So all of it is required -- the one observed error
// code, our own format in the request, both buffers present, and no device
// filter (a request carrying one is a different question being asked).
inline bool retriesModeInEightBit(unsigned hr, bool insideOurTransaction,
                                  unsigned requestFormat, bool haveRequest, bool haveSelected,
                                  bool hasDeviceFilter) {
    if (!insideOurTransaction) return false;
    if (hr != kErrorNotFound) return false;
    if (!haveRequest || !haveSelected) return false;
    if (hasDeviceFilter) return false;
    return requestFormat == kRgba16Float;
}

// ---- which candidates widen (diagnostic) ----------------------------------

// advanced.hdr_widen: "all" (or empty) widens every candidate the rule above
// admits; otherwise a comma-separated list of candidate numbers, 1-based in
// creation order, as the "hdr: target #N" log lines number them. A number
// outside 1..64 or a word that is not a number is ignored. Returns true for
// "all"; otherwise *mask has bit (n-1) set for each listed n.
inline bool parseWidenList(const char* text, unsigned long long* mask) {
    *mask = 0;
    if (!text) return true;
    const char* p = text;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == 0) return true;
    if ((p[0] == 'a' || p[0] == 'A') && (p[1] == 'l' || p[1] == 'L') &&
        (p[2] == 'l' || p[2] == 'L') && (p[3] == 0 || p[3] == ' '))
        return true;
    while (*p) {
        unsigned n = 0;
        bool digits = false;
        while (*p >= '0' && *p <= '9') { n = n * 10 + unsigned(*p - '0'); digits = true; ++p; if (n > 1000) n = 1000; }
        if (digits && n >= 1 && n <= 64) *mask |= 1ull << (n - 1);
        while (*p && *p != ',') ++p;
        if (*p == ',') ++p;
        while (*p == ' ' || *p == '\t') ++p;
    }
    return false;
}

inline bool widenSelected(bool all, unsigned long long mask, unsigned candidate) {
    if (all) return true;
    return candidate >= 1 && candidate <= 64 && (mask >> (candidate - 1)) & 1ull;
}

// ---- the tone target, learned ---------------------------------------------
// Only the texture the tone shader draws into carries HDR values to the final
// copy (the 2026-11-11 census: every other candidate holds 8-bit content). Its
// creation order is not stable between sessions, so it is recognised by its
// shape instead: the census-proven tone target is learned at its first tone
// draw, saved, and from then on only creates of that shape are widened.

// advanced.hdr_widen.
enum class WidenMode { Auto, All, List };

// "auto" (or empty, or unset): learned shapes. "all": every candidate.
// Anything else: a candidate list (parseWidenList).
inline WidenMode parseWidenMode(const char* text, unsigned long long* mask) {
    *mask = 0;
    if (!text) return WidenMode::Auto;
    const char* p = text;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == 0) return WidenMode::Auto;
    auto word = [&](const char* w) {
        unsigned i = 0;
        for (; w[i]; ++i) {
            char c = p[i];
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            if (c != w[i]) return false;
        }
        return p[i] == 0 || p[i] == ' ' || p[i] == '\t';
    };
    if (word("auto")) return WidenMode::Auto;
    if (word("all")) return WidenMode::All;
    parseWidenList(text, mask);
    return WidenMode::List;
}

// A texture's shape, everything a create asks for except the format.
struct TextureShape {
    unsigned width = 0, height = 0, bindFlags = 0, mipLevels = 0, miscFlags = 0;
    unsigned sampleCount = 1, arraySize = 1;
    bool operator==(const TextureShape& o) const {
        return width == o.width && height == o.height && bindFlags == o.bindFlags &&
               mipLevels == o.mipLevels && miscFlags == o.miscFlags &&
               sampleCount == o.sampleCount && arraySize == o.arraySize;
    }
};

constexpr unsigned kMaxToneShapes = 8;

// One line of the saved file: "tone W H bind mips misc samples array".
inline bool parseShapeLine(const char* line, TextureShape* out) {
    unsigned v[7] = {};
    const char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (!(p[0] == 't' && p[1] == 'o' && p[2] == 'n' && p[3] == 'e' && (p[4] == ' ' || p[4] == '\t')))
        return false;
    p += 4;
    for (int i = 0; i < 7; ++i) {
        while (*p == ' ' || *p == '\t') ++p;
        bool digits = false;
        unsigned long long n = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + unsigned(*p - '0'); digits = true; ++p; if (n > 0xFFFFFFFFull) return false; }
        if (!digits) return false;
        v[i] = unsigned(n);
    }
    if (v[0] == 0 || v[1] == 0 || v[5] == 0 || v[6] == 0) return false;
    *out = TextureShape{v[0], v[1], v[2], v[3], v[4], v[5], v[6]};
    return true;
}

// Adds `shape` at the front (most recent first), dropping a duplicate and the
// oldest beyond kMaxToneShapes. Returns true when the list changed.
inline bool rememberShape(TextureShape* shapes, unsigned* count, const TextureShape& shape) {
    unsigned at = *count;
    for (unsigned i = 0; i < *count; ++i) if (shapes[i] == shape) { at = i; break; }
    if (at == 0 && *count > 0) return false;
    unsigned last = at < *count ? at : (*count < kMaxToneShapes ? *count : kMaxToneShapes - 1);
    for (unsigned i = last; i > 0; --i) shapes[i] = shapes[i - 1];
    shapes[0] = shape;
    if (at == *count && *count < kMaxToneShapes) ++*count;
    return true;
}

// Whether a candidate (one rules::widens admits) is widened in Auto mode.
// Learning -- nothing saved yet, or a miss this session -- widens everything.
inline bool widenInAuto(const TextureShape* shapes, unsigned count, const TextureShape& shape,
                        bool learning) {
    if (learning || count == 0) return true;
    for (unsigned i = 0; i < count; ++i) if (shapes[i] == shape) return true;
    return false;
}

// ---- brightness ----------------------------------------------------------

// The three luminances the HDR shaders are built with, in nits.
//   scene: where the tone curve puts diffuse white (the "paper white").
//   ui:    where 1.0 of the game's 2D interface lands, and EDVR's menu with it.
//   peak:  where the tone curve's shoulder ends.
// PQ is absolute, and DXMT presents it at absolute luminance whatever the
// brightness slider says, so these are what the panel emits.
struct HdrNits {
    float scene = 0.0f;
    float ui = 0.0f;
    float peak = 0.0f;
    bool peakFromDisplay = false;  // peak came from the output's MaxLuminance
};

constexpr float kDefaultSceneNits = 261.0f;   // RenoDX Elite preset's diffuse white
constexpr float kDefaultUiNits = 500.0f;      // RenoDX Elite preset's graphics white
constexpr float kFallbackPeakNits = 1000.0f;  // no usable display report, no ini value
constexpr float kMinWhiteNits = 80.0f;
constexpr float kMaxWhiteNits = 2000.0f;
constexpr float kMinPeakNits = 400.0f;
constexpr float kMaxPeakNits = 10000.0f;
// The shoulder needs room above diffuse white; below this ratio it is a clip.
constexpr float kMinPeakOverScene = 1.25f;

inline float clampNits(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// iniScene/iniUi/iniPeak are the configured values, 0 (or less) meaning
// "not set". displayPeak is DXGI_OUTPUT_DESC1::MaxLuminance, 0 when it could
// not be read. A configured peak wins; otherwise the display's report is used
// when it is plausible, else kFallbackPeakNits.
inline HdrNits resolveNits(float iniScene, float iniUi, float iniPeak, float displayPeak) {
    HdrNits n;
    n.scene = iniScene > 0.0f ? clampNits(iniScene, kMinWhiteNits, kMaxWhiteNits)
                              : kDefaultSceneNits;
    n.ui = iniUi > 0.0f ? clampNits(iniUi, kMinWhiteNits, kMaxWhiteNits) : kDefaultUiNits;
    if (iniPeak > 0.0f) {
        n.peak = clampNits(iniPeak, kMinPeakNits, kMaxPeakNits);
    } else if (displayPeak >= kMinPeakNits && displayPeak <= kMaxPeakNits) {
        n.peak = displayPeak;
        n.peakFromDisplay = true;
    } else {
        n.peak = kFallbackPeakNits;
    }
    if (n.peak < n.scene * kMinPeakOverScene) n.peak = n.scene * kMinPeakOverScene;
    return n;
}

}  // namespace hdrrules
}  // namespace edvr
