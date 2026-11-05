#pragma once

// THE HDR SHADERS, AS EDHDR PROVED THEM.
//
// fix.hdr runs the four shaders the separate EDHDR proxy ran, and this header
// carries their HLSL verbatim -- byte for byte, not a paraphrase. Their
// constants were fitted against RenoDX's installed Elite preset as an oracle
// (edelite-hdr/docs/FP16-INTERNAL-RESOURCE-SPEC.md) and the comment in the
// tone shader says plainly not to retune them by eye. A rewrite here would
// throw away the only validation they have, so when EDHDR's copy changes the
// way to update this one is to copy the file again and re-record its hash
// below.
//
// Provenance, ~/GitHub/edelite-hdr/src, sha256 of each file as copied:
//   tone_hdr_ps.hlsl   c807279f5abfaf8f4416ebd58fe9aba44383ae2de2679a073d38702a68b81056
//   copyscale_ps.hlsl  6ca9396b77c006a909f958f1e33bb1e358b578e11430c6ca75b8f59c4530c07a
//   outdecode_ps.hlsl  0b57034cc3c7031c92b62b71d412a38b9e9f70687991a62216f97b9488cbf6c0
//   outdecode_vs.hlsl  5f32b67d8272764475eba0eefb8d03fc3696aebc87180e107a16b52d64f31ccc
//
// Entry point is "m" in all four, which is EDHDR's convention and not this
// repository's "main": the text is unedited, so the compile calls in
// hdr_output.cpp name "m".
//
// These are compiled by d3dcompiler_47 when the device attaches, not built
// into the DLL by tools/temporal_shader_build the way every other fixed
// shader here is (shader_swap.h, "Fixed HLSL belongs in the build"). That is
// a deliberate exception for the first cut of fix.hdr and it is written down
// in docs/edvr-unified-hdr-2026-11-01.md: four shaders at device attach is
// not the first-Present storm that rule was made for, and keeping the text
// unedited is what makes the port checkable against EDHDR. Moving them into
// the build tool is one Variant entry each.

namespace edvr {

// tone_hdr_ps.hlsl
// Elite's HDR-to-display tonemap, replaced. Display-referred gamma-2.2 BT.709
// with 1.0 = 261 content nits and a ceiling near 2.27 encoded, which is why the
// target it writes has to be wider than 8 bits (hdrOutputWidenTexture).
inline constexpr char kHdrToneReplacementPs[] = R"HLSL(
// tone_hdr_ps.hlsl - HDR tone replacement for Elite Dangerous, transcribed from
// the public RenoDX Elite addon (github.com/clshortfuse/renodx, MIT).
//
// Substituted at h_CreatePS for the engine's real tone pixel shader, matched on
// canonical CRC32 identity (comparable to the RenoDX identity space):
//     len   3192
//     CRC32 D7E646E3
//     FNV   75028D69C8E1B8E9
//
// RenoDX replaces this exact shader too. From
// src/games/elitedangerous/addon.cpp:
//     UpgradeRTVReplaceShader(0xD7E646E3),  // tonemap
//     UpgradeRTVReplaceShader(0x6BE69EF3),  // tonemap, no bloom
//     UpgradeRTVReplaceShader(0xEBD83636),  // tonemap, no bloom, no lut
// with the replacement source at
// src/games/elitedangerous/tonemap/tonemap_0xD7E646E3.ps_5_0.hlsl.
//
// That file is a 3DMigoto transpile of the vanilla shader: it reproduces the
// vanilla scene-referred chain EXACTLY (which our own bytecode decode also
// found) and then replaces the display-referred stages. We match it, up to the
// one deliberate difference documented under ADAPTATION below.
//
// ENTRYPOINT CONTRACT: the runtime D3DCompile call in src/edhdr.cpp (see
// toneReplacementFor) passes the entry-point name "m". This function name must
// stay "m": if it does not, D3DCompile fails with X3501 'entrypoint not found,
// and the proxy silently falls back to Elite's original SDR bytecode.
// tools/check_tone_contract.py and test/run_stage8_tone_replace_test.sh enforce it.
//
// ===========================================================================
// ADAPTATION TO DXMT LINEAR scRGB
// ===========================================================================
// RenoDX presents R10G10B10A2_UNORM as HDR10: swapchain_v1.hpp SetUseHDR10()
// sets target_color_space = hdr10_st2084 and the swap chain is told
// DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, and its swap-chain proxy shader
// finishes with renodx::color::pq::EncodeSafe(color, graphics_white_nits).
//
// We present R16G16B16A16_FLOAT, which macOS consumes as LINEAR scRGB where 1.0
// is SDR white. Two consequences, and they are different things:
//
//   1. We DROP the PQ encode and the BT.709 -> BT.2020 gamut conversion. scRGB
//      is linear; writing PQ code values into a linear target would be doubly
//      wrong.
//   2. We KEEP the trailing gamma encode, because that is not part of the HDR
//      target's encoding - it is the domain Elite's ENTIRE backbuffer is already
//      in. See the comment at the encode below. The matching decode happens
//      exactly once, at Present, in the OUTDECODE pass. That is RenoDX's
//      FinalizeOutput slot, and it is also what covers the video shader, the UI
//      overlay and the 1AD30D44 composite, none of which we replace.
//
// The nits ratio transfers with no fudge: RenoDX's peak/diffuse is a ratio of
// two nit values, and in scRGB 1.0 IS the 203 nit diffuse-white reference, so
// 1589/261 = 6.0881 is already expressed in the units our target expects.
//
// ===========================================================================
// THE TRANSFORM (RenoDX tone_map_type == 1, "RenoDX (Vanilla+)")
// ===========================================================================
// RenoDX's own default is type 2 ("Custom", a large ICtCp/LMS psychov mapper in
// tonemap/psychov/customtest31.hlsli). We implement type 1 instead: it is small,
// self-contained, entirely available in source, and it is the stage that answers
// the specific failure we hit - the vanilla curve compresses everything above
// ~0.03 into a 1% band, which is why preserving it produced an SDR-looking image.
//
//   y0(x)          vanilla rational curve, tokens 481-583 of our dump.
//                  Same coefficients RenoDX keeps in ApplyVanillaTonemap.
//   Vanilla+       Compute y0 with N4 = 0, which removes vanilla's black clip,
//                  gamma-decode it to linear, then RELEASE it onto a tangent
//                  line with the curve's own inflection slope. The release
//                  weight decays as exp2(-k / d^2), so below the inflection the
//                  vanilla response is untouched and above it the response
//                  becomes LINEAR instead of asymptotic. That is the entire
//                  reason the RenoDX image reads as HDR: at x = 1 vanilla yields
//                  0.91 while this yields ~1.85.
//   shoulder       ApplyAnchoredCInfinityShoulder toward peak_white_nits /
//                  diffuse_white_nits, anchored at MID_GRAY_OUT.
//
// ===========================================================================
// NOT INCLUDED (and why)
// ===========================================================================
//   * RenoDX's t0 3D LUT and its cb2[11].x residual mix. RenoDX samples it in
//     gamma space with a max-channel scale that depends on the tonemap type; at
//     the captured cb2[11].x = 0.044 the effect is ~4%. It changes colour
//     slightly, not image formation, so it stays out of this iteration.
//   * RenoDX's display-gamma stage SignPow(y, cb2[9].z * 2.2). Our output domain
//     is linear, so applying a gamma would double-encode. Elite's own cb2[9].z
//     was 1/2.2 (exponent 1.0) in the captured frame anyway.
//   * cb2[10].z saturation. Identity at the captured value of 1.0, and it is a
//     colour op we have not validated in this domain.
//   * bloom tap t2/s2. This one we DO keep, and it is the proven vanilla term.
//
// ===========================================================================
// SCENE-REFERRED CHAIN (vanilla, byte-for-byte)
// ===========================================================================
//   s = clamp(t1.Sample(s1,uv) * cb2[2].x, 0, 64500*cb2[2].x)   tokens 47-73
//   b = clamp(t2.Sample(s2,uv) * cb2[2].x, 0, 64500*cb2[2].x)   tokens 91-109
//   f = autoExposure(v0.z)                                       tokens 116-267
//   x = f * (bloom*b + scene*s) via the cb2[1].w blend          tokens 414-446
//   x = clamp(x, 0, 64500*cb2[2].x) * cb2[6].y                  tokens 456-473
//
// Alpha is written as 1.0: the original wrote t1's alpha scaled by exposure,
// which is not a valid coverage value, and downstream consumers discard on
// alpha ~= 0. This pass has no HDR effect on alpha.
SamplerState      s1 : register(s1);
SamplerState      s2 : register(s2);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
cbuffer           cb2 : register(b2) { float4 cb2[16]; };

// --- Working-oracle values from ED_RENO's installed ReShade preset ---
// ReShade.ini [renodx-preset1]: ToneMapPeakNits=1589, ToneMapGameNits=261,
// ToneMapUINits=500, ToneMapType=2. NOT the public-source defaults
// (1000/203/203). Ratio 1589/261 = 6.0881226. Algorithm below is unchanged;
// only these two constants move to the installed configuration.
#define PEAK_WHITE_NITS     1589.0
#define DIFFUSE_WHITE_NITS  261.0
// peak expressed in units where 1.0 == diffuse white == scRGB SDR white
#define PEAK_RATIO          (PEAK_WHITE_NITS / DIFFUSE_WHITE_NITS)

// --- RenoDX tonemap.hlsli inflection / release constants ---
#define INFLECTION_X                0.119121851127f   // == MID_GRAY_IN
#define INFLECTION_Y                0.163979921774f   // == MID_GRAY_OUT
#define INFLECTION_SLOPE            1.95752308422f
#define NATURAL_RELEASE_EXP2_NUM    0.0386582117104f
#define SHOULDER_COMPRESSION        1.5f
#define ANCHOR                      INFLECTION_Y

// Elite's display transfer exponent. RenoDX's gamma::EncodeSafe/DecodeSafe are
// called with 2.2 throughout its tone chain, GameScaleAndGrain and
// FinalizeOutput, and Elite's own SDR output is on the same 2.2 convention.
// Must equal the exponent the OUTDECODE pass uses (src/outdecode_ps.hlsl); the
// coupling is asserted by test/run_stage10_out_domain_test.sh.
#define DISPLAY_GAMMA               2.2

// vanilla rational curve literals, tokens 481-583 (== RenoDX N*/D*)
#define N0  8.46800041f
#define N1  1.0f
#define N2 -0.00295699993f
#define N3  0.000100400001f
#define N4  0.0f                 // RenoDX passes 0 here to remove the black clip
#define D0  8.3604002f
#define D1  1.82270002f
#define D2  0.218899995f
#define D3 -0.00211700005f
#define D4  3.67300017e-5f

#define FP16_HEADROOM 64500.0

// RenoDX auto-exposure scalar, tokens 116-267. The v0.z guard is ours: the
// original has none, but it feeds log2() and a divide, so a zero would poison
// the frame. Clamped far outside any plausible engine value.
float autoExposure(float v0z) {
    float vz = clamp(v0z, 1e-4, 1e4);
    float r = (cb2[8].z != 0.0) ? 1.4 : 0.0;   // 116
    r += cb2[8].x;                             // 132
    r += log2(vz * 8.0);                       // 140-152
    r -= 3.0;                                  // 160
    float g = cb2[7].w + cb2[7].w;             // 167
    r *= cb2[8].y;                             // 176
    r  = exp2(r);                              // 184
    r += 1.0;                                  // 189
    r  = log2(r);                              // 196
    r  = r * 0.30103 + g;                      // 201
    r  = g / r;                                // 210
    r  = log2(r);                              // 217
    r *= cb2[7].w;                             // 222
    r  = exp2(r);                              // 230
    r  = cb2[7].z * 1.03 - r;                  // 235
    r /= vz;                                   // 246
    r  = log2(r);                              // 253
    r -= cb2[7].y;                             // 258
    return exp2(r);                            // 267
}

// RenoDX ApplyVanillaTonemap with N4 = 0.
float3 eliteCurveNoClip(float3 x) {
    float3 num = (((x * N0 + N1) * x + N2) * x + N3) * x + N4;
    float3 den = (((x * D0 + D1) * x + D2) * x + D3) * x + D4;
    return max(num / den, 0.0);
}

// RenoDX ApplyExtendedVanillaTonemap: gamma-decode the vanilla curve to linear,
// then release it onto the tangent at the curve's own inflection point. The
// exp2(-k/d^2) weight is ~1 for large d, so above the inflection the response
// is the straight line, not the curve. Returns LINEAR values.
float3 vanillaPlus(float3 x) {
    float3 vanilla_gamma = eliteCurveNoClip(x);
    float3 vanilla_linear = pow(vanilla_gamma, 2.2);
    float3 d = max(x - INFLECTION_X, 0.0);
    float3 tangent_linear = INFLECTION_SLOPE * d + INFLECTION_Y;
    float3 release_weight = exp2(-NATURAL_RELEASE_EXP2_NUM / max(d * d, 1e-30));
    return max(vanilla_linear + (tangent_linear - vanilla_linear) * release_weight, 0.0);
}

// RenoDX ApplyAnchoredCInfinityShoulder from common.hlsli: identity through the
// anchor and its derivative, then approaching peak monotonically and concave
// down. Per channel, so hue is preserved.
float3 anchoredShoulder(float3 color, float peak, float anchor, float strength) {
    float3 shoulder_range = peak - anchor;
    float3 dist = max(color - anchor, 0.0);
    float3 flat_weight = exp2(-shoulder_range / (strength * dist));
    float3 denom = mad(dist, flat_weight, shoulder_range);
    return mad(shoulder_range, dist / denom, color - dist);
}

float4 m(float3 v0 : TEXCOORD1) : SV_Target {
    float E = cb2[2].x;                                  // engine exposure

    float3 s = t1.Sample(s1, v0.xy).rgb;                 // full-res scene
    float3 b = t2.Sample(s2, v0.xy).rgb;                 // quarter-res bloom

    s = clamp(s * E, 0.0, FP16_HEADROOM * E);            // tokens 47-73
    b = clamp(b * E, 0.0, FP16_HEADROOM * E);            // tokens 91-109

    float f = autoExposure(v0.z);                        // tokens 116-267

    // Bloom / scene blend, tokens 414-446.
    float w  = cb2[1].w;
    float bw = (w >= 0.0) ? w        : 1.0;
    float sw = (w >= 0.0) ? (1.0 - w) : 1.0;
    float3 sceneLin = f * (bw * b + sw * s);

    sceneLin  = clamp(sceneLin, 0.0, FP16_HEADROOM * E); // tokens 456-466
    sceneLin *= cb2[6].y;                                // token 473, channel gain

    // ---- RenoDX display transform, keeping its gamma encode ----
    float3 y = vanillaPlus(sceneLin);
    y = anchoredShoulder(y, PEAK_RATIO, ANCHOR, SHOULDER_COMPRESSION);

    y *= cb2[10].x;                                      // token 712, output scale

    if (cb2[15].y != 0.0) y = 0.0;                       // token 740, fade out

    // Gamma ENCODE, exactly as RenoDX's tonemap.hlsli ends
    // ApplyPostLUTToneMap:  return gamma::EncodeSafe(tonemapped, 2.2f);
    //
    // This is NOT cosmetic, it is the domain contract. Everything else in
    // Elite's backbuffer is display-referred (gamma 2.2):
    //   - the vanilla tone pass wrote a viewFmt=28 UNORM view, so the ROP
    //     clamped to [0,1] and stored display-referred values; its own
    //     cb2[9].z*2.2 stage is 0.454545*2.2 = 1.0, i.e. a no-op, because the
    //     y0 quartic is already a display-referred curve;
    //   - 2F04633E (video) ends in an exp2/gamma encode;
    //   - the menu/UI arrives as a separate BC1 overlay (t0, fmt70) that the
    //     1AD30D44 composite writes out unchanged.
    // Emitting scene-linear from this one writer while every other writer
    // emits display-referred values puts two domains in one buffer, and macOS
    // reads R16G16B16A16_FLOAT as LINEAR scRGB where 1.0 is SDR white, so a
    // mid-grey that Elite meant as 0.21 of white arrives as 0.5 of white.
    // The single decode to linear happens once, at Present, in the OUTDECODE
    // pass (OUTPUT_DECODE in src/edhdr.cpp) - RenoDX's FinalizeOutput slot.
    y = pow(max(y, 0.0), 1.0 / DISPLAY_GAMMA);
    return float4(y, 1.0);
})HLSL";

// copyscale_ps.hlsl
// The game's final-copy passthrough, replaced: it rescales the tone shader's
// 261-nit diffuse-white unit into the 500-nit graphics-white unit the output
// stage encodes from. Alpha is carried through unchanged.
inline constexpr char kHdrCopyScalePs[] = R"HLSL(
// copyscale_ps.hlsl - RenoDX-equivalent GameScale for the D6A846C8 copy path.
//
// Substituted at h_CreatePS for Elite's scene-copy pixel shader, matched on
// runtime-proven identity (from our own capture, vanillaplus-ed071ae-run1.log):
//     len   268
//     CRC32 D6A846C8
//     FNV   3C4D865B7031B44C
// The vanilla shader is a pure passthrough (sample t0; ret), confirmed by
// disassembly and by BACKWRITE srv=[27,-2,-2,-2] (it samples the fmt27 tone
// target and writes the backbuffer).
//
// RenoDX replaces this exact shader at runtime (ReShade.log PipelineShaderDetails
// "Replacing pipeline for 0xd6a846c8") with copy_0xD6A846C8.ps_5_0.hlsl, which is
// the vanilla passthrough plus one call:
//
//     o0.rgb = GameScaleAndGrain(o0.rgb, v0.xy);
//
// This file is that call, transcribed. RenoDX's GameScaleAndGrain
// (src/games/elitedangerous/common.hlsli) is:
//
//     color = gamma::DecodeSafe(color, 2.2f);
//     color = ApplyFilmGrain(...);                    // strength 0 here: omitted
//     if (type != 0) { BT2020 round-trip clamp at PEAK/DIFFUSE }
//     color *= DIFFUSE_WHITE_NITS / GRAPHICS_WHITE_NITS;
//     color = gamma::EncodeSafe(color, 2.2f);
//
// Omitted pieces, and why:
//   * Film grain: installed FxGrainStrength=0, so RenoDX multiplies by
//     CUSTOM_GRAIN_STRENGTH * 0.03 = 0. Do not invent grain.
//   * The BT2020 round-trip peak clamp: it clamps at PEAK/DIFFUSE = 6.0889,
//     far above anything the copy path carries in practice, and it exists to
//     bound the tonemap output, not to shape the copy. The tone shoulder
//     already bounds our output below it. Left out deliberately; the clamp
//     value is asserted by test, not applied here, so a future need is visible.
//
// WHAT THIS IS FOR. The tone shader emits display-referred values in
// diffuse-white units (1.0 = game diffuse white = 261 nits). The final output
// transform (OUTDECODE, RenoDX's FinalizeOutput slot) interprets the buffer in
// graphics-white units (1.0 = UI white = 500 nits). Without this scale the two
// ends disagree by 261/500 and every scene pixel arrives ~2x too bright in
// display-referred terms (~4x after the decode). This scale is the unit
// conversion between them. It MUST happen in linear light - DecodeSafe, scale,
// EncodeSafe - exactly as RenoDX orders it. Scaling the gamma-encoded value
// directly would be a different (wrong) operation.
//
// Alpha passes through untouched, exactly as RenoDX's copy does.
//
// ENTRYPOINT CONTRACT: compiled by copyReplacementFor() in src/edhdr.cpp with
// entry "m" and profile ps_5_0. The name must stay "m".
// test/run_stage11_gamescale_test.sh enforces it.
Texture2D<float4> t0 : register(t0);
SamplerState      s0_s : register(s0);

// Working-oracle values from ED_RENO's installed ReShade.ini [renodx-preset1].
// NOT the public-source defaults (1000/203/203). Do not retune by eye.
#define DIFFUSE_WHITE_NITS   261.0
#define GRAPHICS_WHITE_NITS  500.0
#define DISPLAY_GAMMA        2.2

float4 m(float2 v0 : TEXCOORD0) : SV_Target {
    float4 c = t0.Sample(s0_s, v0.xy);

    // RenoDX GameScaleAndGrain, grain omitted (installed strength 0):
    float3 lin = pow(max(c.rgb, 0.0), DISPLAY_GAMMA);   // DecodeSafe
    lin *= DIFFUSE_WHITE_NITS / GRAPHICS_WHITE_NITS;    // 261/500 = 0.522
    float3 enc = pow(max(lin, 0.0), 1.0 / DISPLAY_GAMMA); // EncodeSafe

    return float4(enc, c.a);
}
)HLSL";

// outdecode_ps.hlsl
// The output stage's transform. OUTDECODE_PQ=1 gives ST 2084 / BT.2020 with
// 1.0 in = 500 nits; OUTDECODE_PQ=0 gives linear scRGB with OUTDECODE_GAIN.
inline constexpr char kHdrOutDecodePs[] = R"HLSL(
// Final output only. The scratch remains Elite's gamma-2.2 encoded BT.709
// image; D7E/D6 and their FP16 resource chain are unchanged.
// Legacy mode: decode -> linear BT.709/scRGB, with optional presentation gain.
// PQ experiment: Reno FinalizeOutput semantics -> ST 2084 / BT.2020.
// PQ code values REQUIRE DXGI RGB_FULL_G2084_NONE_P2020; format FP16 alone
// does not establish the transfer function. Production enables this branch
// only alongside a successful SetColorSpace1. See output-domain audit.
Texture2D<float4> t0 : register(t0);
SamplerState      s0 : register(s0);
#define DISPLAY_GAMMA 2.2
#ifndef OUTDECODE_GAIN
#define OUTDECODE_GAIN 1.0
#endif
#ifndef OUTDECODE_PQ
#define OUTDECODE_PQ 0
#endif

#if OUTDECODE_PQ
// D65 linear BT.709 -> D65 linear BT.2020. Product of Reno's
// XYZ_TO_BT2020_MAT and BT709_TO_XYZ_MAT (pinned source in audit).
static const float3x3 TO_2020 = float3x3(
    0.627403896, 0.329283038, 0.043313066,
    0.069097289, 0.919540395, 0.011362316,
    0.016391439, 0.088013308, 0.895595253);
float3 outputPQ(float3 encoded709) {
    // Reno DecodeSafe is sign-preserving. Clamp after conversion, as in
    // PQ EncodeSafe; clamping before the matrix changes mixed-sign colors.
    float3 linear709 = sign(encoded709) * pow(abs(encoded709), DISPLAY_GAMMA);
    float3 linear2020 = mul(TO_2020, linear709);
    // Absolute CONTENT nits enter here: pre-PQ 1 == graphics white 500 nits.
    // No scRGB gain or H/P compensation in this branch. DXMT handles PQ.
    float3 p = pow(max(linear2020, 0.0) * (500.0 / 10000.0), 2610.0 / 16384.0);
    return pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) /
               (1.0 + (2392.0 / 128.0) * p), 2523.0 / 32.0);
}
#endif

float4 m(float4 pos : SV_Position) : SV_Target {
    // Identical-size point load; Boundary F state/routing/write masks retained.
    float4 c = t0.Load(int3(int2(pos.xy), 0));
#if OUTDECODE_PQ
    return float4(outputPQ(c.rgb), 1.0);
#else
    float3 lin = pow(max(c.rgb, 0.0), DISPLAY_GAMMA);
    return float4(lin * OUTDECODE_GAIN, 1.0);
#endif
}
)HLSL";

// outdecode_vs.hlsl
// The output stage's fullscreen triangle, from SV_VertexID, so the pass needs no
// input layout and no vertex buffer.
inline constexpr char kHdrOutDecodeVs[] = R"HLSL(
// outdecode_vs.hlsl - fullscreen triangle for the OUTDECODE pass. No vertex
// buffer: positions come from SV_VertexID, so the pass needs no input layout
// object and no per-draw vertex state from the caller.
//
// Covers clip space with one oversized triangle (-1,-1) (3,-1) (-1,3). Texel
// coordinates come from SV_Position in the pixel shader, so no interpolation is needed
// and the pass is exact at any backbuffer size.
//
// ENTRYPOINT CONTRACT: compiled with entry "m" and profile vs_5_0 by
// outputDecodeShaders() in src/edhdr.cpp. The name must stay "m".
// test/run_stage10_out_domain_test.sh enforces it.
// ===========================================================================
static const float2 kFullscreen[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
};

float4 m(uint vid : SV_VertexID) : SV_Position {
    return float4(kFullscreen[vid], 0.0, 1.0);
}
)HLSL";

}  // namespace edvr
