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

}  // namespace hdrrules
}  // namespace edvr
