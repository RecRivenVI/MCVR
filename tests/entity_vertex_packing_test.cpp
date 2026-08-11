#include "core/vulkan/vertex.hpp"
#include <cstring>
#include <iostream>

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
    std::cout << "Direct aggregate entity packing equals existing conversion across layers\n";
}
