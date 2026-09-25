#include "core/vulkan/vertex.hpp"
#include <cstring>
#include <iostream>
#include <array>
#include <stdexcept>

int main() {
    std::vector<vk::VertexFormat::PBRVertex> source(257);
    for (size_t i = 0; i < source.size(); ++i) {
        auto &v = source[i];
        v.pos = glm::vec3(float(i), -float(i), float(i) * 0.25f);
        v.norm = glm::vec3(0, 1, 0);
        v.textureID = uint32_t(i % 4095);
        v.textureUV = glm::vec2(float(i) / 257.0f, 0.75f);
        v.useColorLayer = i % 3; v.useTexture = 1; v.useOverlay = i % 2;
        v.alphaMode = i % 20; v.coordinate = i % 3; v.useGlint = i % 4;
        v.colorLayer = glm::vec4(float(i)*0.1f, 0.25f, -0.5f, float(i%11)/10.0f);
        v.overlayUV = glm::ivec2(i%16, (i+3)%16);
        v.glintUV = glm::vec2(-0.75f,float(i)*0.02f); v.glintTexture = i%4095;
        v.albedoEmission = float(i%5); v.lightUV = glm::ivec2(i%256,(i+1)%256);
        v.useNorm = i%2; v.useLight = (i+1)%2;
    }
    auto expectedPositions = vk::Vertex::buildPositionVertices(source);
    auto expectedMaterials = vk::Vertex::buildMaterialVertices(source);
    for (auto &v : expectedMaterials) v.emissiveOverlayTextureID = 71;
    std::vector<vk::VertexFormat::PositionVertex> positions;
    std::vector<vk::VertexFormat::MaterialVertex> materials;
    positions.reserve(514); materials.reserve(514);
    vk::Vertex::appendPackedVertices(source, 71, positions, materials);
    vk::Vertex::appendPackedVertices(source, 71, positions, materials);
    if (positions.size() != 514 || materials.size() != 514) return 1;
    for (size_t i = 0; i < 514; ++i) {
        const auto &a = materials[i], &b = expectedMaterials[i % 257];
        if (positions[i].pos != expectedPositions[i % 257].pos || positions[i].pad0 != 0 ||
            a.norm != b.norm || a.textureID != b.textureID || a.textureUV != b.textureUV ||
            a.colorLayer != b.colorLayer || a.overlayUV != b.overlayUV || a.glintUV != b.glintUV ||
            a.glintTexture != b.glintTexture || a.albedoEmission != b.albedoEmission ||
            a.lightUV != b.lightUV || a.packedData != b.packedData ||
            a.emissiveOverlayTextureID != b.emissiveOverlayTextureID) return 2;
    }
    using P = vk::VertexFormat::PositionVertex;
    using M = vk::VertexFormat::MaterialVertex;
    std::vector<P> directPositions(516);
    std::vector<M> directMaterials(516);
    std::memset(directPositions.data(),0x5a,directPositions.size()*sizeof(P));
    std::memset(directMaterials.data(),0x5a,directMaterials.size()*sizeof(M));
    const auto pGuard=directPositions.front(); const auto mGuard=directMaterials.front();
    auto pOut=std::span(directPositions).subspan(1,514);
    auto mOut=std::span(directMaterials).subspan(1,514);
    vk::Vertex::writePackedVertices(source,71,pOut.first(257),mOut.first(257));
    vk::Vertex::writePackedVertices(source,19,pOut.last(257),mOut.last(257));
    for(size_t i=0;i<514;++i) {
        auto expected=expectedMaterials[i%257]; expected.emissiveOverlayTextureID=i<257?71:19;
        if(std::memcmp(&pOut[i],&expectedPositions[i%257],sizeof(P)) ||
           std::memcmp(&mOut[i],&expected,sizeof(M))) return 3;
    }
    if(std::memcmp(&directPositions.front(),&pGuard,sizeof(P)) ||
       std::memcmp(&directPositions.back(),&pGuard,sizeof(P)) ||
       std::memcmp(&directMaterials.front(),&mGuard,sizeof(M)) ||
       std::memcmp(&directMaterials.back(),&mGuard,sizeof(M))) return 4;
    const auto before=directPositions; const auto beforeMaterials=directMaterials;
    bool rejected=false;
    try { vk::Vertex::writePackedVertices(source,0,pOut.first(256),mOut.first(257)); }
    catch(const std::invalid_argument &) { rejected=true; }
    if(!rejected || std::memcmp(before.data(),directPositions.data(),before.size()*sizeof(P)) ||
       std::memcmp(beforeMaterials.data(),directMaterials.data(),beforeMaterials.size()*sizeof(M))) return 5;
    vk::Vertex::writePackedVertices({},0,{},{});
    std::cout << "Aggregate and mapped-stream packing preserve every field, layer offsets, guards and failure bounds\n";
}
