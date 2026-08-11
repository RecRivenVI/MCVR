#include "common/pbr_source.hpp"
#include <bit>
#include <iostream>
#include <limits>
#include <fstream>

int main(int argc, char **argv) {
    using namespace mcvr;
    for (uint32_t i=0; i<4096; ++i) {
        vk::VertexFormat::PBRVertex v{};
        v.pos={std::bit_cast<float>(0x80000000u),float(i)*0.17f,-float(i)};
        v.norm={-1.25f,0.375f,5.5f};
        v.colorLayer={float(i)/255.f,-2.f,4.f,float(i%257)/17.f};
        v.textureUV={-900.25f,65536.75f}; v.overlayUV={INT32_MIN,INT32_MAX};
        v.textureID=0xffffffffu-i; v.glintUV={1.0e-30f,-32769.125f};
        v.glintTexture=i; v.lightUV={-12345,876543};
        v.albedoEmission=std::bit_cast<float>(i%2 ? 0x7fc12345u : 0x80000000u);
        v.postBase={0.125f,-456.5f,1.0e20f};
        v.useNorm=i&1; v.useColorLayer=(i>>1)&3; v.useTexture=(i>>3)&1;
        v.useOverlay=(i>>4)&1; v.useGlint=(i>>5)&3; v.useLight=(i>>7)&1;
        v.coordinate=i&255; v.alphaMode=(i>>4)&255;
        auto packed=encodeCompactPbr(v);
        auto decoded=decodeCompactPbr(std::as_bytes(std::span(packed)));
        if (std::memcmp(&v,&decoded,sizeof(v))) return 1;
        packed[24]|=0x01000000u;
        try { decodeCompactPbr(std::as_bytes(std::span(packed))); return 2; }
        catch (const std::invalid_argument &) {}
        try { decodeCompactPbr(std::as_bytes(std::span(packed)).first(99)); return 3; }
        catch (const std::invalid_argument &) {}
    }
    vk::VertexFormat::PBRVertex invalid{}; invalid.coordinate=256;
    try { encodeCompactPbr(invalid); return 4; } catch (const std::invalid_argument &) {}
    if (pbrSourceStride(12)!=128 || pbrSourceStride(13)!=100) return 5;
    try { pbrSourceStride(14); return 6; } catch (const std::invalid_argument &) {}
    if(pbrSourceWords(0,0)!=32 || pbrSourceWords(0,32)!=32 || pbrSourceWords(1,25)!=25) return 11;
    for(auto pair: {std::array<uint32_t,2>{1,32}, {0,25}, {2,25}}) {
        try { pbrSourceWords(pair[0],pair[1]); return 12; } catch(const std::invalid_argument &) {}
    }
    if (argc > 1) {
        std::ifstream stream(argv[1],std::ios::binary);
        std::array<uint32_t,3> header{};
        stream.read(reinterpret_cast<char *>(header.data()),sizeof(header));
        if (!stream || header[0]!=0x31524250u || header[1]!=1 || !header[2] || header[2]>4096)
            return 7;
        for (uint32_t i=0; i<header[2]; ++i) {
            vk::VertexFormat::PBRVertex legacy;
            CompactPbrSource compact;
            stream.read(reinterpret_cast<char *>(&legacy),sizeof(legacy));
            stream.read(reinterpret_cast<char *>(compact.data()),sizeof(compact));
            if (!stream) return 8;
            auto decoded=decodeCompactPbr(std::as_bytes(std::span(compact)));
            if (std::memcmp(&legacy,&decoded,sizeof(legacy))) return 9;
        }
        if (stream.peek()!=std::char_traits<char>::eof()) return 10;
        std::cout << "Actual Java legacy/compact producer records match native decoding: " << header[2] << '\n';
    }
    std::cout << "Compact source v1 preserves every field bit and rejects unsupported input\n";
}
