#pragma once
#include "shared.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

namespace mcvr {
// Versioned CPU input ABI. Final BLAS/material streams keep their existing layouts.
inline constexpr int legacyPbrSourceFormat = 12;
inline constexpr int compactPbrSourceFormat = 13;
inline constexpr uint32_t compactPbrSourceVersion = 1;
inline constexpr uint32_t compactPbrSourceBytes = 100;
using CompactPbrSource = std::array<uint32_t, 25>;
static_assert(sizeof(CompactPbrSource) == compactPbrSourceBytes);
static_assert(sizeof(vk::VertexFormat::PBRVertex) == 128);

inline uint32_t pbrSourceStride(int format) {
    if (format == legacyPbrSourceFormat) return 128;
    if (format == compactPbrSourceFormat) return compactPbrSourceBytes;
    throw std::invalid_argument("Unknown PBR source format");
}

inline uint32_t pbrSourceWords(uint32_t version, uint32_t declaredWords) {
    if (version == 0 && (declaredWords == 0 || declaredWords == 32)) return 32;
    if (version == compactPbrSourceVersion && declaredWords == 25) return 25;
    throw std::invalid_argument("PBR source job version/stride mismatch");
}

// This encoder is a test/reference utility, never a per-frame 128 -> 100 repacking step.
inline CompactPbrSource encodeCompactPbr(const vk::VertexFormat::PBRVertex &v) {
    if (v.useNorm > 1 || v.useColorLayer > 3 || v.useTexture > 1 || v.useOverlay > 1 ||
        v.useGlint > 3 || v.useLight > 1 || v.coordinate > 255 || v.alphaMode > 255)
        throw std::invalid_argument("PBR discrete field exceeds compact v1 domain");
    CompactPbrSource out{};
    std::memcpy(out.data(), &v.pos, 12);
    std::memcpy(out.data()+3, &v.norm, 12);
    std::memcpy(out.data()+6, &v.colorLayer, 16);
    std::memcpy(out.data()+10, &v.textureUV, 8);
    std::memcpy(out.data()+12, &v.overlayUV, 8);
    out[14]=v.textureID;
    std::memcpy(out.data()+15, &v.glintUV, 8);
    out[17]=v.glintTexture;
    std::memcpy(out.data()+18, &v.lightUV, 8);
    std::memcpy(out.data()+20, &v.albedoEmission, 4);
    std::memcpy(out.data()+21, &v.postBase, 12);
    out[24]=v.useNorm | (v.useColorLayer<<1) | (v.useTexture<<3) | (v.useOverlay<<4) |
        (v.useGlint<<5) | (v.useLight<<7) | (v.coordinate<<8) | (v.alphaMode<<16);
    return out;
}

inline vk::VertexFormat::PBRVertex decodeCompactPbr(std::span<const std::byte> input) {
    if (input.size() != compactPbrSourceBytes)
        throw std::invalid_argument("Invalid compact PBR source length");
    CompactPbrSource words;
    std::memcpy(words.data(), input.data(), sizeof(words));
    const uint32_t flags=words[24];
    if (flags & 0xff000000u) throw std::invalid_argument("Unsupported compact PBR source flags");
    vk::VertexFormat::PBRVertex v{};
    std::memcpy(&v.pos, words.data(), 12);
    std::memcpy(&v.norm, words.data()+3, 12);
    std::memcpy(&v.colorLayer, words.data()+6, 16);
    std::memcpy(&v.textureUV, words.data()+10, 8);
    std::memcpy(&v.overlayUV, words.data()+12, 8);
    v.textureID=words[14];
    std::memcpy(&v.glintUV, words.data()+15, 8);
    v.glintTexture=words[17];
    std::memcpy(&v.lightUV, words.data()+18, 8);
    std::memcpy(&v.albedoEmission, words.data()+20, 4);
    std::memcpy(&v.postBase, words.data()+21, 12);
    v.useNorm=flags&1u; v.useColorLayer=(flags>>1)&3u;
    v.useTexture=(flags>>3)&1u; v.useOverlay=(flags>>4)&1u;
    v.useGlint=(flags>>5)&3u; v.useLight=(flags>>7)&1u;
    v.coordinate=(flags>>8)&255u; v.alphaMode=(flags>>16)&255u;
    return v;
}
}
