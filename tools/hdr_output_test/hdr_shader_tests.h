#pragma once
#include "../../src/d3d11/hdr_output_shaders.h"
#include "texture_lifetime_tests.h"
#include "aa_memory_tests.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace hdr_shader_tests {
using Microsoft::WRL::ComPtr;

inline ComPtr<ID3DBlob> compile(const char* source, const char* entry, const char* profile,
                               const D3D_SHADER_MACRO* macros = nullptr) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "HDR regression", macros, nullptr,
                                 entry, profile, 0, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        code.Reset();
    }
    return code;
}

template<class Check> void run(Check check, bool hardware) {
    for (bool noBloom : {false, true}) {
        const D3D_SHADER_MACRO macros[] = {{"HDR_TONE_NO_BLOOM", "1"}, {nullptr, nullptr}};
        auto code = compile(edvr::kHdrToneReplacementPs, "m", "ps_5_0", noBloom ? macros : nullptr);
        check(code != nullptr, "production HDR tone variant compiles");
        if (!code) continue;
        ComPtr<ID3D11ShaderReflection> reflection;
        const HRESULT hr = D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
                                       IID_ID3D11ShaderReflection, &reflection);
        check(SUCCEEDED(hr), "tone bytecode reflects");
        if (!reflection) continue;
        D3D11_SHADER_INPUT_BIND_DESC desc{};
        check(SUCCEEDED(reflection->GetResourceBindingDescByName("t1", &desc)) && desc.BindPoint == 1,
              "tone variant reads scene t1");
        check(SUCCEEDED(reflection->GetResourceBindingDescByName("s1", &desc)) && desc.BindPoint == 1,
              "tone variant reads sampler s1");
        check(SUCCEEDED(reflection->GetResourceBindingDescByName("cb2", &desc)) && desc.BindPoint == 2,
              "tone variant reads engine b2");
        const bool texture = SUCCEEDED(reflection->GetResourceBindingDescByName("t2", &desc));
        const bool sampler = SUCCEEDED(reflection->GetResourceBindingDescByName("s2", &desc));
        check(texture == !noBloom && sampler == !noBloom,
              "only the bloom variant reads t2/s2; no-bloom never reads absent bloom bindings");
    }
    auto copy = compile(edvr::kHdrCopyScalePs, "m", "ps_5_0");
    check(copy != nullptr, "production copy conversion compiles");

    // Execute the production exposure function, not a second HLSL copy of it.
    // The reference below follows the captured no-bloom DXBC operations.
    std::string source = edvr::kHdrToneReplacementPs;
    source += "\nRWStructuredBuffer<float> result : register(u0);\n"
              "[numthreads(1,1,1)] void exposureTest(uint3 id:SV_DispatchThreadID)"
              "{ result[0] = autoExposure(1.0); }\n";
    auto code = compile(source.c_str(), "exposureTest", "cs_5_0");
    check(code != nullptr, "production exposure function compiles for execution");
    if (!code) return;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const HRESULT hr = D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    check(SUCCEEDED(hr), "exposure regression device");
    if (!device) return;
    texture_lifetime_tests::runDevice(check, device.Get());
    texture_lifetime_tests::runCapture(check, device.Get());
    aa_memory_tests::runDevice(check, device.Get());
    ComPtr<ID3D11ComputeShader> shader;
    if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) {
        check(false, "exposure compute shader"); return;
    }
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 256; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants, output, staging;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &constants))) { check(false, "exposure constants"); return; }
    desc.ByteWidth = 4; desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; desc.StructureByteStride = 4;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &output))) { check(false, "exposure output"); return; }
    desc.BindFlags = 0; desc.MiscFlags = 0; desc.StructureByteStride = 0;
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &staging))) { check(false, "exposure readback"); return; }
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; view.Buffer.NumElements = 1;
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (FAILED(device->CreateUnorderedAccessView(output.Get(), &view, &uav))) { check(false, "exposure UAV"); return; }
    for (bool boosted : {false, true}) {
        float cb[16][4]{};
        cb[7][1] = 0.25f; cb[7][2] = 2.0f; cb[7][3] = 1.0f;
        cb[8][0] = 0.7f; cb[8][1] = 1.0f; cb[8][2] = boosted ? 1.0f : 0.0f;
        context->UpdateSubresource(constants.Get(), 0, nullptr, cb, 0, 0);
        context->CSSetShader(shader.Get(), nullptr, 0);
        ID3D11Buffer* b = constants.Get(); context->CSSetConstantBuffers(2, 1, &b);
        ID3D11UnorderedAccessView* u = uav.Get(); context->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        context->Dispatch(1, 1, 1);
        u = nullptr; context->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT read = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        check(SUCCEEDED(read), "exposure readback completes");
        if (FAILED(read)) continue;
        const float actual = *static_cast<const float*>(mapped.pData);
        context->Unmap(staging.Get(), 0);
        const double bias = 0.7 + (boosted ? 1.4 : 0.0);
        const double divisor = std::log2(1.0 + std::exp2(-bias)) * 0.30103 + 2.0;
        const double expected = (2.0 * 1.03 - 2.0 / divisor) * std::exp2(-0.25);
        check(std::isfinite(actual) && std::abs(actual - expected) < 1e-5,
              "exposure subtracts bias, matching captured engine DXBC");
    }
}
} // namespace hdr_shader_tests
