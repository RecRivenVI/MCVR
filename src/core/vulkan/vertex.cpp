#include "core/vulkan/vertex.hpp"

#include "common/shared.hpp"

uint32_t vk::Vertex::packMaterialFlags(const VertexFormat::PBRVertex &vertex) {
    uint32_t packed = 0;
    packed |= vertex.useColorLayer > 0 ? useColorLayerBit : 0u;
    packed |= vertex.useTexture > 0 ? useTextureBit : 0u;
    packed |= vertex.useOverlay > 0 ? useOverlayBit : 0u;
    packed |= vertex.useGlint > 0 ? useGlintBit : 0u;
    packed |= vertex.useNorm > 0 ? useNormBit : 0u;
    packed |= vertex.useLight > 0 ? useLightBit : 0u;
    packed |= vertex.useColorLayer > 1 ? colorLayerMixBit : 0u;
    packed |= (vertex.alphaMode & 0x1Fu) << alphaModeShift;
    packed |= (vertex.coordinate & 0xFu) << coordinateShift;
    packed |= (vertex.useGlint & 0x3u) << glintModeShift;
    return packed;
}

vk::VertexFormat::PositionVertex vk::Vertex::makePositionVertex(const VertexFormat::PBRVertex &vertex) {
    return {
        .pos = vertex.pos,
        .pad0 = 0,
    };
}

vk::VertexFormat::MaterialVertex vk::Vertex::makeMaterialVertex(const VertexFormat::PBRVertex &vertex) {
    return {
        .norm = vertex.norm,
        .textureID = vertex.textureID,
        .colorLayer = vertex.colorLayer,
        .textureUV = vertex.textureUV,
        .overlayUV = vertex.overlayUV,
        .glintUV = vertex.glintUV,
        .glintTexture = vertex.glintTexture,
        .albedoEmission = vertex.albedoEmission,
        .lightUV = vertex.lightUV,
        .packedData = packMaterialFlags(vertex),
        .emissiveOverlayTextureID = 0,
    };
}

std::vector<vk::VertexFormat::PositionVertex>
vk::Vertex::buildPositionVertices(const std::vector<VertexFormat::PBRVertex> &vertices) {
    std::vector<VertexFormat::PositionVertex> packedVertices;
    packedVertices.reserve(vertices.size());
    for (const auto &vertex : vertices) { packedVertices.push_back(makePositionVertex(vertex)); }
    return packedVertices;
}

void vk::Vertex::appendPackedVertices(const std::vector<VertexFormat::PBRVertex> &vertices,
                                     uint32_t emissiveOverlay,
                                     std::vector<VertexFormat::PositionVertex> &positions,
                                     std::vector<VertexFormat::MaterialVertex> &materials) {
    for (const auto &vertex : vertices) {
        positions.push_back(makePositionVertex(vertex));
        auto material = makeMaterialVertex(vertex);
        material.emissiveOverlayTextureID = emissiveOverlay;
        materials.push_back(material);
    }
}

std::vector<vk::VertexFormat::MaterialVertex>
vk::Vertex::buildMaterialVertices(const std::vector<VertexFormat::PBRVertex> &vertices) {
    std::vector<VertexFormat::MaterialVertex> packedVertices;
    packedVertices.reserve(vertices.size());
    for (const auto &vertex : vertices) { packedVertices.push_back(makeMaterialVertex(vertex)); }
    return packedVertices;
}

template <>
vk::VertexLayoutInfo &vk::Vertex::vertexLayoutInfo<vk::VertexFormat::Triangle>() {
    static std::vector<VertexAttribute> attributes = {
        {VK_FORMAT_R32G32B32_SFLOAT, offsetof(vk::VertexFormat::Triangle, pos)},
        {VK_FORMAT_R32G32B32_SFLOAT, offsetof(vk::VertexFormat::Triangle, color)},
    };

    static vk::VertexLayoutInfo vertexLayoutInfo = initVertexLayout<vk::VertexFormat::Triangle>(attributes);
    return vertexLayoutInfo;
}

template <>
vk::VertexLayoutInfo &vk::Vertex::vertexLayoutInfo<vk::VertexFormat::TexturedTriangle>() {
    static std::vector<VertexAttribute> attributes = {
        {VK_FORMAT_R32G32B32_SFLOAT, offsetof(vk::VertexFormat::TexturedTriangle, pos)},
        {VK_FORMAT_R32G32_SFLOAT, offsetof(vk::VertexFormat::TexturedTriangle, uv)},
    };
    static vk::VertexLayoutInfo vertexLayoutInfo = initVertexLayout<vk::VertexFormat::TexturedTriangle>(attributes);
    return vertexLayoutInfo;
}

template <>
vk::VertexLayoutInfo &vk::Vertex::vertexLayoutInfo<vk::VertexFormat::ArrayTexturedTriangle>() {
    static std::vector<VertexAttribute> attributes = {
        {VK_FORMAT_R32G32B32_SFLOAT, offsetof(vk::VertexFormat::ArrayTexturedTriangle, pos)},
        {VK_FORMAT_R32G32_SFLOAT, offsetof(vk::VertexFormat::ArrayTexturedTriangle, uv)},
        {VK_FORMAT_R32_SFLOAT, offsetof(vk::VertexFormat::ArrayTexturedTriangle, textureLayer)},
    };
    static vk::VertexLayoutInfo vertexLayoutInfo = initVertexLayout<vk::VertexFormat::ArrayTexturedTriangle>(attributes);
    return vertexLayoutInfo;
}
