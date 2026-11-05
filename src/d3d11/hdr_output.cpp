// fix.hdr's implementation. hdr_output.h says what this replaces and why; what
// follows is how, in the order the frame meets it: configuration, the
// swapchain, the widened targets and their views, the two substituted pixel
// shaders, and the output transform at Present.
#include "hdr_output.h"

#include <windows.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string.h>   // _stricmp

#include "../common/config.h"
#include "../common/log.h"
#include "hdr_output_rules.h"
#include "hdr_shader_identity.h"
#include "hdr_output_shaders.h"

namespace edvr {
namespace {

// hdr_output_rules.h writes the two predicates in plain integers so a test can
// run them without d3d11.h. These hold those integers to the real enumerators,
// so a rule and the API it decides for can never drift apart.
namespace rules = hdrrules;
static_assert(rules::kRgba16Typeless == DXGI_FORMAT_R16G16B16A16_TYPELESS &&
                  rules::kRgba16Float == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                  rules::kRgba8Typeless == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                  rules::kRgba8Unorm == DXGI_FORMAT_R8G8B8A8_UNORM &&
                  rules::kRgba8UnormSrgb == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
                  rules::kRgba8Uint == DXGI_FORMAT_R8G8B8A8_UINT &&
                  rules::kRgba8Snorm == DXGI_FORMAT_R8G8B8A8_SNORM &&
                  rules::kRgba8Sint == DXGI_FORMAT_R8G8B8A8_SINT &&
                  rules::kBgra8Unorm == DXGI_FORMAT_B8G8R8A8_UNORM &&
                  rules::kBgrx8Unorm == DXGI_FORMAT_B8G8R8X8_UNORM &&
                  rules::kBgra8Typeless == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
                  rules::kBgra8UnormSrgb == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
              "the rules header's format codes must be the DXGI ones");
static_assert(rules::kBindShaderResource == D3D11_BIND_SHADER_RESOURCE &&
                  rules::kBindRenderTarget == D3D11_BIND_RENDER_TARGET &&
                  rules::kBindUnorderedAccess == D3D11_BIND_UNORDERED_ACCESS &&
                  rules::kMiscShared == D3D11_RESOURCE_MISC_SHARED &&
                  rules::kMiscSharedKeyedMutex == D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX &&
                  rules::kMiscSharedNtHandle == D3D11_RESOURCE_MISC_SHARED_NTHANDLE &&
                  rules::kUsageStaging == D3D11_USAGE_STAGING,
              "the rules header's bind, misc and usage bits must be the D3D11 ones");

rules::TextureAsk askOf(const D3D11_TEXTURE2D_DESC& desc) {
    rules::TextureAsk ask;
    ask.format = desc.Format;
    ask.bindFlags = desc.BindFlags;
    ask.miscFlags = desc.MiscFlags;
    ask.usage = desc.Usage;
    ask.cpuAccessFlags = desc.CPUAccessFlags;
    ask.sampleCount = desc.SampleDesc.Count;
    ask.arraySize = desc.ArraySize;
    return ask;
}

// The presented format. Display-referred HDR has to be able to carry values
// above 1.0 all the way to the transform at Present -- the tone shader's
// ceiling is about 2.27 in encoded units -- and FP16 is the only swapchain
// format DXMT offers that can (d3d11_swapchain.cpp ConvertSwapChainFormat:
// BGRA8, BGRA8_sRGB, RGB10A2 and RGBA16F, and the first three are all UNORM).
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// The widened form of the game's post-tone targets. TYPELESS, like the format
// it replaces, so the game's own views keep choosing the interpretation; the
// view hook maps the 8-bit ones to R16G16B16A16_FLOAT.
constexpr DXGI_FORMAT kWideTypeless = DXGI_FORMAT_R16G16B16A16_TYPELESS;
constexpr DXGI_FORMAT kWideFloat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// ---- state ---------------------------------------------------------------

struct State {
    bool on = false;
    bool wantPq = true;          // advanced.hdr_encode
    bool effectivePq = false;    // what the colour space actually accepted
    bool domainDirty = true;     // re-tag on the next Present (a resize clears the tag)
    bool shadersFailed = false;
    bool encodeFailed = false;

    ID3D11Device* device = nullptr;

    ID3D11PixelShader* tonePs = nullptr;
    ID3D11PixelShader* toneNoBloomPs = nullptr;
    ID3D11PixelShader* copyPs = nullptr;

    // The transform, in place.
    ID3D11ComputeShader* encodeCs = nullptr;
    ID3D11UnorderedAccessView* backUav = nullptr;

    // The transform, out of place: the fallback for a backbuffer that will not
    // take a UAV.
    ID3D11VertexShader* drawVs = nullptr;
    ID3D11PixelShader* drawPs = nullptr;
    ID3D11Texture2D* scratch = nullptr;
    ID3D11ShaderResourceView* scratchSrv = nullptr;
    ID3D11RenderTargetView* backRtv = nullptr;
    ID3D11BlendState* drawBlend = nullptr;
    ID3D11DepthStencilState* drawDepth = nullptr;
    ID3D11RasterizerState* drawRaster = nullptr;

    ID3D11Texture2D* backBuffer = nullptr;
    UINT backWidth = 0, backHeight = 0;
    // The size the in-place shader's bounds were baked for. A resize makes
    // these disagree with backWidth/backHeight and forces a recompile.
    unsigned csWidth = 0, csHeight = 0;

    unsigned widened = 0;        // textures widened in place
    unsigned inexactViews = 0;   // view formats mapped without an exact match
    unsigned passes = 0;
};

State g;

constexpr unsigned kInexactViewNotes = 8;
constexpr unsigned kPassNotes = 4;

// ---- shader compilation --------------------------------------------------

using PfnD3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                       void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                       ID3DBlob**);

PfnD3DCompile compiler() {
    static PfnD3DCompile fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        // The one beside the game, the way EDHDR loaded it, and never freed:
        // the shaders it compiles outlive the module handle's usefulness.
        HMODULE module = LoadLibraryA("d3dcompiler_47.dll");
        if (module) fn = reinterpret_cast<PfnD3DCompile>(
                reinterpret_cast<void*>(GetProcAddress(module, "D3DCompile")));
        if (!fn) Log::get().note("hdr: d3dcompiler_47.dll has no D3DCompile; HDR stands down");
    }
    return fn;
}

// Compile an HDR shader, entry point "m" (EDHDR's convention).
// Returns a blob the caller releases, or null with the
// compiler's own message in the log.
ID3DBlob* compileHlsl(const char* source, const char* name, const char* profile,
                      const char* entry, const D3D_SHADER_MACRO* macros) {
    PfnD3DCompile compile = compiler();
    if (!compile) return nullptr;
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(source, std::strlen(source), name, macros, nullptr, entry,
                               profile, 0, 0, &blob, &errors);
    if (FAILED(hr) || !blob) {
        char message[400] = {};
        if (errors && errors->GetBufferPointer())
            std::snprintf(message, sizeof message, "%.390s",
                          static_cast<const char*>(errors->GetBufferPointer()));
        Log::get().note("hdr: %s (%s, entry \"%s\") would not compile, 0x%08lX: %s", name,
                        profile, entry, static_cast<unsigned long>(hr), message);
        if (errors) errors->Release();
        if (blob) blob->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    return blob;
}

// The output transform as a compute shader over the backbuffer itself. The
// transform is EDHDR's pixel shader text with its input adapter swapped, the
// way EDHDR's own in-place build did it (output_uav_source.h), so there is no
// second copy of the gamma, gamut and PQ equations to keep in step.
//
// The bounds are baked in as macros, which is EDHDR's form and not a choice.
// The obvious alternative -- ask the image its own size with
// RWTexture2D::GetDimensions, so a resize needs no recompile -- is what the
// first cut did, and the 2026-11-01 flight refused it:
//
//   outdecode_cs:50:16: Method 'GetDimensions' is not defined on type
//   'RWTexture2D<float4>'
//
// That is not Microsoft's compiler talking. The d3dcompiler_47 this runs
// against is Wine's, and its HLSL front end does not implement the
// resource-info methods on a UAV type. The compile failed, the in-place path
// stood down, and the output stage ran at twice the bandwidth as a copy and a
// fullscreen draw -- a correct picture, which is why only the log showed it.
// Baked bounds compile everywhere; a resize rebuilds the shader instead
// (prepareDomain), which costs one compile at a resize and nothing in a frame.
bool encodeComputeSource(std::string& out) {
    const char* signature = "float4 m(float4 pos : SV_Position) : SV_Target {";
    const char* load = "float4 c = t0.Load(int3(int2(pos.xy), 0));";
    out = kHdrOutDecodePs;
    const auto sig = out.find(signature);
    const auto read = out.find(load);
    if (sig == std::string::npos || read == std::string::npos || read < sig ||
        out.find(signature, sig + 1) != std::string::npos ||
        out.find(load, read + 1) != std::string::npos) {
        Log::get().note("hdr: the output transform's text is not the shape the compute "
                        "wrapper edits; in-place encode stands down");
        return false;
    }
    out.erase(read, std::strlen(load));
    out.replace(sig, std::strlen(signature), "float4 edvrHdrEncode(float4 c) {");
    out +=
        "\n"
        "RWTexture2D<float4> edvrHdrImage : register(u0);\n"
        "[numthreads(8, 8, 1)]\n"
        "void edvrHdrCompute(uint3 id : SV_DispatchThreadID) {\n"
        "    if (id.x >= EDVR_HDR_OUTPUT_WIDTH || id.y >= EDVR_HDR_OUTPUT_HEIGHT) return;\n"
        "    edvrHdrImage[id.xy] = edvrHdrEncode(edvrHdrImage[id.xy]);\n"
        "}\n";
    return true;
}

void releaseEncodeTargets() {
    if (g.backUav) { g.backUav->Release(); g.backUav = nullptr; }
    if (g.scratchSrv) { g.scratchSrv->Release(); g.scratchSrv = nullptr; }
    if (g.scratch) { g.scratch->Release(); g.scratch = nullptr; }
    if (g.backRtv) { g.backRtv->Release(); g.backRtv = nullptr; }
}

void releaseBackBuffer() {
    releaseEncodeTargets();
    if (g.backBuffer) { g.backBuffer->Release(); g.backBuffer = nullptr; }
    g.backWidth = g.backHeight = 0;
    g.csWidth = g.csHeight = 0;
}

// The output transform's three shaders, for one domain. The domain is a
// compile-time branch in the HLSL (OUTDECODE_PQ), so a fall back from PQ to
// linear scRGB is a rebuild, not a flag flip -- which is why this is a
// function and not a stretch of hdrOutputAttach.
bool buildTransformShaders(bool pq) {
    if (!g.device) return false;
    releaseEncodeTargets();
    if (g.encodeCs) { g.encodeCs->Release(); g.encodeCs = nullptr; }
    g.csWidth = g.csHeight = 0;
    if (g.drawPs) { g.drawPs->Release(); g.drawPs = nullptr; }
    if (g.drawVs) { g.drawVs->Release(); g.drawVs = nullptr; }

    char gain[32];
    std::snprintf(gain, sizeof gain, "%.9g", 1.0);
    const D3D_SHADER_MACRO macros[] = {{"OUTDECODE_GAIN", gain},
                                       {"OUTDECODE_PQ", pq ? "1" : "0"},
                                       {nullptr, nullptr}};
    ID3DBlob* ps = compileHlsl(kHdrOutDecodePs, "outdecode_ps", "ps_5_0", "m", macros);
    ID3DBlob* vs = compileHlsl(kHdrOutDecodeVs, "outdecode_vs", "vs_5_0", "m", nullptr);
    bool ok = ps && vs;
    if (ok) {
        ok = SUCCEEDED(g.device->CreatePixelShader(ps->GetBufferPointer(),
                                                   ps->GetBufferSize(), nullptr, &g.drawPs)) &&
             SUCCEEDED(g.device->CreateVertexShader(vs->GetBufferPointer(),
                                                    vs->GetBufferSize(), nullptr, &g.drawVs));
    }
    if (ps) ps->Release();
    if (vs) vs->Release();
    return ok;
}

// The in-place form, for one domain at one size. Optional: without it the
// fullscreen draw is the whole transform, which is what EDHDR had to do.
//
// It is built here and not in buildTransformShaders because its bounds are
// baked in (see encodeComputeSource), so it cannot be compiled until the
// backbuffer's size is known, and it has to be compiled again when that size
// changes. Returns true when the in-place path is available afterwards.
bool buildEncodeCompute(bool pq, unsigned width, unsigned height) {
    if (g.encodeCs && g.csWidth == width && g.csHeight == height) return true;
    if (g.encodeCs) { g.encodeCs->Release(); g.encodeCs = nullptr; }
    if (g.backUav) { g.backUav->Release(); g.backUav = nullptr; }
    g.csWidth = g.csHeight = 0;
    if (!g.device || !width || !height) return false;

    std::string source;
    if (!encodeComputeSource(source)) return false;
    char gain[32], w[16], h[16];
    std::snprintf(gain, sizeof gain, "%.9g", 1.0);
    std::snprintf(w, sizeof w, "%u", width);
    std::snprintf(h, sizeof h, "%u", height);
    const D3D_SHADER_MACRO macros[] = {{"OUTDECODE_GAIN", gain},
                                       {"OUTDECODE_PQ", pq ? "1" : "0"},
                                       {"EDVR_HDR_OUTPUT_WIDTH", w},
                                       {"EDVR_HDR_OUTPUT_HEIGHT", h},
                                       {nullptr, nullptr}};
    ID3DBlob* cs = compileHlsl(source.c_str(), "outdecode_cs", "cs_5_0", "edvrHdrCompute",
                               macros);
    if (!cs) return false;
    if (FAILED(g.device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(),
                                             nullptr, &g.encodeCs)))
        g.encodeCs = nullptr;
    cs->Release();
    if (!g.encodeCs) return false;
    g.csWidth = width;
    g.csHeight = height;
    return true;
}

// ---- the output domain ---------------------------------------------------

// Tag the swapchain. PQ code values mean nothing without
// RGB_FULL_G2084_NONE_P2020: the format alone does not establish a transfer
// function, so the tag and the shader's branch have to agree or the image is
// wrong in a way that looks like a tone bug.
HRESULT applyColorSpace(IDXGISwapChain* swapChain, bool pq, UINT* support) {
    *support = 0;
    IDXGISwapChain3* sc3 = nullptr;
    HRESULT hr = swapChain->QueryInterface(__uuidof(IDXGISwapChain3),
                                          reinterpret_cast<void**>(&sc3));
    if (FAILED(hr) || !sc3) return FAILED(hr) ? hr : E_NOINTERFACE;
    const DXGI_COLOR_SPACE_TYPE space = pq ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                           : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    hr = sc3->CheckColorSpaceSupport(space, support);
    if (SUCCEEDED(hr) && !(*support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = sc3->SetColorSpace1(space);
    sc3->Release();
    return hr;
}

bool ensureBackBuffer(IDXGISwapChain* swapChain) {
    if (g.backBuffer) return true;
    ID3D11Texture2D* texture = nullptr;
    // DXMT refuses GetBuffer for an RTV IID; take the texture.
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&texture))) ||
        !texture)
        return false;
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    if (desc.Format != kHdrFormat) {
        // Not ours: the swapchain was made before the hook, or something
        // downstream refused the format. Leave every pixel alone.
        texture->Release();
        return false;
    }
    g.backBuffer = texture;
    g.backWidth = desc.Width;
    g.backHeight = desc.Height;
    return true;
}

// The in-place path's admission test, and the UAV it needs. Fails quietly into
// the out-of-place path.
bool ensureEncodeUav() {
    if (g.backUav) return true;
    if (!g.device || !g.backBuffer || !g.encodeCs) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    g.backBuffer->GetDesc(&desc);
    if (desc.Format != kHdrFormat || desc.SampleDesc.Count != 1 || desc.MipLevels > 1 ||
        desc.ArraySize != 1)
        return false;
    if (!(desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) return false;
    // A typed FP16 UAV load and store, asked of the runtime rather than
    // assumed: the shader reads the pixel it writes.
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2 = {kHdrFormat, 0};
    if (FAILED(g.device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2,
                                             sizeof support2)))
        return false;
    const UINT wanted = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
                        D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    if ((support2.OutFormatSupport2 & wanted) != wanted) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd = {};
    uavd.Format = kHdrFormat;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(g.device->CreateUnorderedAccessView(g.backBuffer, &uavd, &g.backUav))) {
        g.backUav = nullptr;
        return false;
    }
    return true;
}

// The out-of-place path's scratch copy, its SRV, and an RTV over the real
// backbuffer. The backbuffer cannot be sampled while it is the render target,
// which is the whole reason this path copies.
bool ensureEncodeDrawTargets() {
    if (g.scratch && g.scratchSrv && g.backRtv) return true;
    if (!g.device || !g.backBuffer || !g.drawVs || !g.drawPs) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    g.backBuffer->GetDesc(&desc);
    if (desc.Format != kHdrFormat) return false;
    D3D11_TEXTURE2D_DESC scratch = desc;
    scratch.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    scratch.MiscFlags = 0;
    scratch.CPUAccessFlags = 0;
    scratch.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(g.device->CreateTexture2D(&scratch, nullptr, &g.scratch))) {
        g.scratch = nullptr;
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format = kHdrFormat;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
    rtvd.Format = kHdrFormat;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g.device->CreateShaderResourceView(g.scratch, &srvd, &g.scratchSrv)) ||
        FAILED(g.device->CreateRenderTargetView(g.backBuffer, &rtvd, &g.backRtv))) {
        releaseEncodeTargets();
        return false;
    }
    return true;
}

// Compile and allocate BEFORE tagging, never after: a frame presented under a
// PQ tag without the transform applied is a bright, wrong frame, and that is
// the one failure mode worth structural care.
void prepareDomain(IDXGISwapChain* swapChain) {
    if (!g.domainDirty) return;
    if (!ensureBackBuffer(swapChain)) return;
    g.domainDirty = false;
    const bool wasPq = g.effectivePq;
    // The in-place shader's bounds are baked in, so it is compiled here -- the
    // first point where the backbuffer's size is known -- and again whenever a
    // resize changes it. A failure is not fatal: the draw path below covers it.
    if (!g.shadersFailed) buildEncodeCompute(g.wantPq, g.backWidth, g.backHeight);
    if (g.wantPq && !g.shadersFailed) {
        g.effectivePq = true;
        const bool ready = ensureEncodeUav() || ensureEncodeDrawTargets();
        UINT support = 0;
        const HRESULT hr = ready ? applyColorSpace(swapChain, true, &support) : E_FAIL;
        if (SUCCEEDED(hr)) {
            Log::get().note("hdr: output domain PQ BT.2020, graphics white 500 nits, "
                            "%ux%u format %u, in place=%d (SetColorSpace1 0x%08lX support 0x%X)",
                            g.backWidth, g.backHeight, unsigned(kHdrFormat),
                            g.backUav ? 1 : 0, static_cast<unsigned long>(hr), support);
            return;
        }
        Log::get().note("hdr: PQ is unavailable here (0x%08lX, support 0x%X); falling back "
                        "to linear scRGB", static_cast<unsigned long>(hr), support);
        g.effectivePq = false;
        g.wantPq = false;
        // The domain is a compile-time branch, so the fallback is a rebuild.
        // The tag goes on in the pass below this one, on this same Present.
        if (!buildTransformShaders(false)) {
            g.shadersFailed = true;
            Log::get().note("hdr: the linear output transform would not build either; "
                            "fix.hdr stands down");
            return;
        }
        buildEncodeCompute(false, g.backWidth, g.backHeight);
    }
    UINT support = 0;
    const HRESULT hr = applyColorSpace(swapChain, false, &support);
    if (FAILED(hr) && wasPq) {
        // Never draw linear code values under a retained PQ tag.
        g.encodeFailed = true;
        Log::get().note("hdr: the linear tag would not go back on (0x%08lX); the output "
                        "transform is off for this session", static_cast<unsigned long>(hr));
        return;
    }
    const bool ready = ensureEncodeUav() || ensureEncodeDrawTargets();
    Log::get().note("hdr: output domain linear scRGB, %ux%u format %u, in place=%d, "
                    "targets=%d (SetColorSpace1 0x%08lX support 0x%X)", g.backWidth,
                    g.backHeight, unsigned(kHdrFormat), g.backUav ? 1 : 0, ready,
                    static_cast<unsigned long>(hr), support);
}

// ---- the transform ------------------------------------------------------

// One dispatch over the backbuffer. The state this touches is the state it
// puts back: the render targets (the backbuffer is one of them, so it has to
// come off before it can be a UAV), the compute shader, compute UAV slot 0 and
// the predicate. EDHDR's version saved and cleared 64 OM UAVs, 64 compute UAVs
// and six stages of 128 SRVs because its clone chain could alias any of them.
// In place there is one aliasing resource -- the backbuffer -- and the game has
// it bound as a render target, not as a texture.
bool encodeInPlace(ID3D11DeviceContext* context) {
    if (!ensureEncodeUav()) return false;

    ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    ID3D11ComputeShader* oldCs = nullptr;
    ID3D11UnorderedAccessView* oldUav = nullptr;
    ID3D11Predicate* oldPredicate = nullptr;
    BOOL oldPredicateValue = FALSE;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
    context->CSGetShader(&oldCs, nullptr, nullptr);
    context->CSGetUnorderedAccessViews(0, 1, &oldUav);
    context->GetPredication(&oldPredicate, &oldPredicateValue);

    if (oldPredicate) context->SetPredication(nullptr, FALSE);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11UnorderedAccessView* uav = g.backUav;
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->CSSetShader(g.encodeCs, nullptr, 0);
    context->Dispatch((g.backWidth + 7) / 8, (g.backHeight + 7) / 8, 1);

    // Putting slot 0 back also takes our view off it, which has to happen
    // before the backbuffer can be a render target again.
    context->CSSetShader(oldCs, nullptr, 0);
    context->CSSetUnorderedAccessViews(0, 1, &oldUav, nullptr);
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
    if (oldPredicate) context->SetPredication(oldPredicate, oldPredicateValue);

    for (auto* rtv : oldRtv) if (rtv) rtv->Release();
    if (oldDsv) oldDsv->Release();
    if (oldCs) oldCs->Release();
    if (oldUav) oldUav->Release();
    if (oldPredicate) oldPredicate->Release();
    return true;
}

// Copy the backbuffer out, transform it back in with a fullscreen triangle.
// The path EDHDR had to use, kept for a backbuffer that will not take a UAV.
bool encodeByDraw(ID3D11DeviceContext* context) {
    if (!ensureEncodeDrawTargets()) return false;

    ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    ID3D11BlendState* oldBlend = nullptr;
    FLOAT oldFactors[4] = {};
    UINT oldMask = 0;
    ID3D11DepthStencilState* oldDepth = nullptr;
    UINT oldStencilRef = 0;
    ID3D11RasterizerState* oldRaster = nullptr;
    ID3D11PixelShader* oldPs = nullptr;
    ID3D11VertexShader* oldVs = nullptr;
    ID3D11ShaderResourceView* oldPsSrv = nullptr;
    D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT oldViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_PRIMITIVE_TOPOLOGY oldTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
    context->OMGetBlendState(&oldBlend, oldFactors, &oldMask);
    context->OMGetDepthStencilState(&oldDepth, &oldStencilRef);
    context->RSGetState(&oldRaster);
    context->RSGetViewports(&oldViewportCount, oldViewports);
    context->PSGetShader(&oldPs, nullptr, nullptr);
    context->VSGetShader(&oldVs, nullptr, nullptr);
    context->PSGetShaderResources(0, 1, &oldPsSrv);
    context->IAGetPrimitiveTopology(&oldTopology);

    context->CopyResource(g.scratch, g.backBuffer);
    ID3D11RenderTargetView* target = g.backRtv;
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetDepthStencilState(g.drawDepth, 0);
    context->OMSetBlendState(g.drawBlend, nullptr, 0xFFFFFFFFu);
    context->RSSetState(g.drawRaster);
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(g.backWidth),
                                     static_cast<float>(g.backHeight), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g.drawVs, nullptr, 0);
    context->PSSetShader(g.drawPs, nullptr, 0);
    ID3D11ShaderResourceView* source = g.scratchSrv;
    context->PSSetShaderResources(0, 1, &source);
    context->Draw(3, 0);

    // Targets before textures: binding the output may have unbound an
    // aliasing view.
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
    context->PSSetShaderResources(0, 1, &oldPsSrv);
    context->PSSetShader(oldPs, nullptr, 0);
    context->VSSetShader(oldVs, nullptr, 0);
    context->IASetPrimitiveTopology(oldTopology);
    context->RSSetState(oldRaster);
    context->OMSetDepthStencilState(oldDepth, oldStencilRef);
    context->OMSetBlendState(oldBlend, oldFactors, oldMask);
    if (oldViewportCount) context->RSSetViewports(oldViewportCount, oldViewports);
    else context->RSSetViewports(0, nullptr);

    for (auto* rtv : oldRtv) if (rtv) rtv->Release();
    if (oldDsv) oldDsv->Release();
    if (oldBlend) oldBlend->Release();
    if (oldDepth) oldDepth->Release();
    if (oldRaster) oldRaster->Release();
    if (oldPs) oldPs->Release();
    if (oldVs) oldVs->Release();
    if (oldPsSrv) oldPsSrv->Release();
    return true;
}

}  // namespace

// ---- the hook faces ------------------------------------------------------

void hdrOutputStartup() {
    const Config& cfg = Config::get();
    const std::string mode = cfg.getString("fix.hdr", "off");
    g.on = _stricmp(mode.c_str(), "on") == 0 || _stricmp(mode.c_str(), "1") == 0;
    const std::string encode = cfg.getString("advanced.hdr_encode", "pq");
    const bool linear = _stricmp(encode.c_str(), "linear") == 0;
    g.wantPq = !linear;
    if (!g.on) {
        if (_stricmp(mode.c_str(), "off") != 0)
            Log::get().note("hdr: \"%s\" is not a value this reads; HDR is off (on, off)",
                            mode.c_str());
        return;
    }
    Log::get().note("hdr: on. The swapchain becomes %u (R16G16B16A16_FLOAT) with "
                    "unordered access, Elite's post-tone RGBA8 targets are created wide, "
                    "its tone and final-copy pixel shaders are substituted, and the frame "
                    "is encoded to %s immediately before Present. No separate HDR proxy is "
                    "needed or expected in the chain.",
                    unsigned(kHdrFormat), g.wantPq ? "PQ BT.2020" : "linear scRGB");
}

bool hdrOutputOn() { return g.on; }

bool hdrOutputSwapChainDesc(const DXGI_SWAP_CHAIN_DESC& asked, DXGI_SWAP_CHAIN_DESC* out) {
    if (!g.on || !out) return false;
    *out = asked;
    out->BufferDesc.Format = kHdrFormat;
    out->BufferUsage |= DXGI_USAGE_UNORDERED_ACCESS;
    Log::get().note("hdr: swapchain asked for format %u, created as %u with unordered "
                    "access (%ux%u, %u buffers)", unsigned(asked.BufferDesc.Format),
                    unsigned(kHdrFormat), asked.BufferDesc.Width, asked.BufferDesc.Height,
                    asked.BufferCount);
    return true;
}

bool hdrOutputSwapChainDesc1(const DXGI_SWAP_CHAIN_DESC1& asked, DXGI_SWAP_CHAIN_DESC1* out) {
    if (!g.on || !out) return false;
    *out = asked;
    out->Format = kHdrFormat;
    out->BufferUsage |= DXGI_USAGE_UNORDERED_ACCESS;
    Log::get().note("hdr: swapchain (hwnd) asked for format %u, created as %u with "
                    "unordered access (%ux%u, %u buffers)", unsigned(asked.Format),
                    unsigned(kHdrFormat), asked.Width, asked.Height, asked.BufferCount);
    return true;
}

DXGI_FORMAT hdrOutputResizeFormat(DXGI_FORMAT asked) {
    return g.on ? kHdrFormat : asked;
}

void hdrOutputSwapChainChanged() {
    if (!g.on) return;
    releaseBackBuffer();
    // DXMT reinitialises the FP16 layer as G10/P709 across a resize, so the
    // tag has to be applied again whatever it was before.
    g.domainDirty = true;
}

bool hdrOutputWidenTexture(const D3D11_TEXTURE2D_DESC& asked, D3D11_TEXTURE2D_DESC* out) {
    // The whole predicate is hdr_output_rules.h's: format and bind class, no
    // size test of any kind, and the exclusions it documents.
    if (!g.on || !out || !rules::widens(askOf(asked))) return false;
    *out = asked;
    out->Format = kWideTypeless;
    if (++g.widened <= 8)
        Log::get().note("hdr: target #%u created wide: %ux%u r8g8b8a8_typeless -> "
                        "r16g16b16a16_typeless (bind 0x%X, %u mips)", g.widened, asked.Width,
                        asked.Height, asked.BindFlags, asked.MipLevels);
    return true;
}

bool hdrOutputViewFormat(ID3D11Resource* resource, DXGI_FORMAT asked, DXGI_FORMAT* mapped) {
    if (!g.on || !resource || !mapped) return false;
    if (!rules::eightBitRgba(asked)) return false;   // the cheap test first
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    auto* texture = static_cast<ID3D11Texture2D*>(resource);
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    unsigned chosen = 0;
    bool exact = false;
    if (!rules::viewFormat(desc.Format, asked, &chosen, &exact)) return false;
    *mapped = static_cast<DXGI_FORMAT>(chosen);
    if (!exact && ++g.inexactViews <= kInexactViewNotes)
        Log::get().note("hdr: a view over a widened %ux%u target asked for format %u, which "
                        "is not colour-compatible with r16g16b16a16_float; it was mapped "
                        "anyway rather than refused (a refused create is fatal in Elite). "
                        "That pass reads what EDHDR's clone chain would have let it read "
                        "8-bit.", desc.Width, desc.Height, unsigned(asked));
    return true;
}

const void* hdrOutputViewDescFor(ID3D11Resource* resource, const void* desc, void* scratch,
                                 size_t descBytes) {
    if (!g.on || !resource || !desc || !scratch || descBytes < sizeof(DXGI_FORMAT))
        return desc;
    DXGI_FORMAT mapped = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT asked = DXGI_FORMAT_UNKNOWN;
    std::memcpy(&asked, desc, sizeof asked);
    if (!hdrOutputViewFormat(resource, asked, &mapped)) return desc;
    std::memcpy(scratch, desc, descBytes);
    std::memcpy(scratch, &mapped, sizeof mapped);
    return scratch;
}

void hdrOutputAttach(ID3D11Device* device) {
    if (!g.on || !device || g.device) return;
    g.device = device;

    // The substitutions first, because the game creates its pixel shaders
    // while it loads and a replacement that is not ready by then is a
    // replacement that never happens.
    ID3DBlob* tone = compileHlsl(kHdrToneReplacementPs, "tone_hdr_ps", "ps_5_0", "m", nullptr);
    const D3D_SHADER_MACRO noBloom[] = {{"HDR_TONE_NO_BLOOM", "1"}, {nullptr, nullptr}};
    ID3DBlob* toneNoBloom = compileHlsl(kHdrToneReplacementPs, "tone_hdr_no_bloom_ps",
                                      "ps_5_0", "m", noBloom);
    ID3DBlob* copy = compileHlsl(kHdrCopyScalePs, "copyscale_ps", "ps_5_0", "m", nullptr);
    bool ok = tone && toneNoBloom && copy;
    if (ok) {
        ok = SUCCEEDED(device->CreatePixelShader(tone->GetBufferPointer(),
                                                 tone->GetBufferSize(), nullptr, &g.tonePs)) &&
             SUCCEEDED(device->CreatePixelShader(toneNoBloom->GetBufferPointer(),
                                                 toneNoBloom->GetBufferSize(), nullptr, &g.toneNoBloomPs)) &&
             SUCCEEDED(device->CreatePixelShader(copy->GetBufferPointer(),
                                                 copy->GetBufferSize(), nullptr, &g.copyPs));
    }
    if (tone) tone->Release();
    if (toneNoBloom) toneNoBloom->Release();
    if (copy) copy->Release();
    if (ok) ok = buildTransformShaders(g.wantPq);

    // The fullscreen path's fixed states. No blending -- the transform
    // replaces a pixel, it never modulates it -- no depth, and no culling, so
    // the triangle covers every pixel whatever the game left behind.
    if (ok) {
        D3D11_BLEND_DESC blend = {};
        for (auto& rt : blend.RenderTarget) {
            rt.BlendEnable = FALSE;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        }
        D3D11_DEPTH_STENCIL_DESC depth = {};
        depth.DepthEnable = FALSE;
        depth.StencilEnable = FALSE;
        D3D11_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (FAILED(device->CreateBlendState(&blend, &g.drawBlend)) ||
            FAILED(device->CreateDepthStencilState(&depth, &g.drawDepth)) ||
            FAILED(device->CreateRasterizerState(&raster, &g.drawRaster)))
            ok = false;
    }
    if (!ok) {
        g.shadersFailed = true;
        Log::get().note("hdr: the HDR shaders would not build; fix.hdr stands down and the "
                        "frame is left exactly as the game drew it");
        hdrOutputShutdown();
        g.on = false;
        return;
    }
    // The in-place shader is NOT built here: its bounds are baked in, so it
    // waits for the first Present, where the backbuffer's size is known. Which
    // path actually ran is the "output domain" line's in_place field, and that
    // is the one to read -- this line can only promise the attempt.
    Log::get().note("hdr: shaders ready at device attach -- tone, copy scale and the "
                    "output transform's draw path (%s). The in-place compute form is built "
                    "at the first Present, once the backbuffer's size is known, and the "
                    "\"output domain\" line's in place= says whether it took. No compile "
                    "happens in a steady-state frame.",
                    g.wantPq ? "PQ BT.2020" : "linear scRGB");
}

ID3D11PixelShader* hdrOutputReplacementPs(const void* bytecode, size_t length) {
    if (!g.on || g.shadersFailed || !bytecode || !length) return nullptr;
    ID3D11PixelShader* replacement = nullptr;
    const char* which = nullptr;
    switch (hdrShaderKind(bytecode, length)) {
    case HdrShaderKind::ToneBloom:
        replacement = g.tonePs;
        which = "tone (bloom)";
        break;
    case HdrShaderKind::ToneNoBloom:
        replacement = g.toneNoBloomPs;
        which = "tone (no bloom)";
        break;
    case HdrShaderKind::Copy:
        replacement = g.copyPs;
        which = "final copy";
        break;
    default: break;
    }
    if (!replacement) return nullptr;
    replacement->AddRef();
    static unsigned notes = 0;
    if (++notes <= 4)
        Log::get().note("hdr: the game's %s pixel shader (len %zu crc 0x%08X) was handed "
                        "the HDR replacement; EDVR still records the original hash, so "
                        "every recogniser sees the draw it expects", which, length,
                        hdrShaderIdentity(bytecode, length).crc);
    return replacement;
}

void hdrOutputBeforePresent(IDXGISwapChain* swapChain, ID3D11DeviceContext* context) {
    if (!g.on || g.encodeFailed || g.shadersFailed || !swapChain || !context) return;
    prepareDomain(swapChain);
    if (!ensureBackBuffer(swapChain) || !g.backWidth || !g.backHeight) return;
    const bool inPlace = encodeInPlace(context);
    if (!inPlace && !encodeByDraw(context)) {
        g.encodeFailed = true;
        Log::get().note("hdr: neither output path could run (%ux%u); the frame is presented "
                        "as drawn and this is said once", g.backWidth, g.backHeight);
        return;
    }
    if (++g.passes <= kPassNotes)
        Log::get().note("hdr: output transform #%u applied %ux%u %s (display-referred -> %s)",
                        g.passes, g.backWidth, g.backHeight,
                        inPlace ? "in place" : "by copy and draw",
                        g.effectivePq ? "PQ BT.2020, graphics white 500 nits"
                                      : "linear scRGB");
    if (g.passes == kPassNotes)
        Log::get().note("hdr: the output transform is running every frame and goes quiet "
                        "here");
}

void hdrOutputShutdown() {
    releaseBackBuffer();
    if (g.tonePs) { g.tonePs->Release(); g.tonePs = nullptr; }
    if (g.toneNoBloomPs) { g.toneNoBloomPs->Release(); g.toneNoBloomPs = nullptr; }
    if (g.copyPs) { g.copyPs->Release(); g.copyPs = nullptr; }
    if (g.encodeCs) { g.encodeCs->Release(); g.encodeCs = nullptr; }
    if (g.drawVs) { g.drawVs->Release(); g.drawVs = nullptr; }
    if (g.drawPs) { g.drawPs->Release(); g.drawPs = nullptr; }
    if (g.drawBlend) { g.drawBlend->Release(); g.drawBlend = nullptr; }
    if (g.drawDepth) { g.drawDepth->Release(); g.drawDepth = nullptr; }
    if (g.drawRaster) { g.drawRaster->Release(); g.drawRaster = nullptr; }
    g.device = nullptr;
    g.domainDirty = true;
}

}  // namespace edvr
