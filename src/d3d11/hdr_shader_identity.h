#pragma once
#include <cstddef>
#include <cstdint>

namespace edvr {
enum class HdrShaderKind { None, ToneBloom, ToneNoBloom, Copy };

struct HdrShaderIdentity {
    size_t bytes;
    uint32_t crc;
    uint64_t fnv;
};

// These identities came from standard FNV-1a-64. EDVR's global fnv1a64 uses
// a historical, different offset basis and must not change: its shader
// recognizers depend on that convention.
inline HdrShaderIdentity hdrShaderIdentity(const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xffffffffu;
    uint64_t fnv = 14695981039346656037ull;
    for (size_t i = 0; i < bytes; ++i) {
        fnv = (fnv ^ p[i]) * 1099511628211ull;
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return {bytes, crc ^ 0xffffffffu, fnv};
}

inline HdrShaderKind hdrShaderMatch(const HdrShaderIdentity& id) {
    if (id.bytes == 3192 && id.crc == 0xD7E646E3u && id.fnv == 0x75028D69C8E1B8E9ull)
        return HdrShaderKind::ToneBloom;
    // Captured no-bloom variant: b2, scene t1/s1; no t2/s2 bloom input.
    if (id.bytes == 2244 && id.crc == 0x6BE69EF3u && id.fnv == 0x3B86E25519AA3DA6ull)
        return HdrShaderKind::ToneNoBloom;
    if (id.bytes == 268 && id.crc == 0xD6A846C8u && id.fnv == 0x3C4D865B7031B44Cull)
        return HdrShaderKind::Copy;
    return HdrShaderKind::None;
}

inline HdrShaderKind hdrShaderKind(const void* data, size_t bytes) {
    if (!data || (bytes != 3192 && bytes != 2244 && bytes != 268))
        return HdrShaderKind::None;
    return hdrShaderMatch(hdrShaderIdentity(data, bytes));
}
} // namespace edvr
