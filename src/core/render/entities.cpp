#include "core/render/material_faces.hpp"
#include "core/render/entities.hpp"

#include "core/logging.hpp"

#include "core/render/buffers.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/world_mesh_contract.hpp"
#include "core/vulkan/vertex.hpp"
#include "core/render/renderer.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>

using Vertex = glm::vec3;
using Triangle = std::array<Vertex, 3>;
using VertexIdentifier = std::array<uint32_t, 2>;
using TriangleIdentifier = std::array<VertexIdentifier, 3>;

namespace {

const char *postRenderFlagName(int postRenderFlag) {
    switch (postRenderFlag) {
        case 0b0001: return "WEATHER";
        case 0b0010: return "PARTICLE";
        case 0b0100: return "TEXT";
        case 0b100000: return "NAME_TAG_SEE_THROUGH";
        default: return "UNKNOWN";
    }
}

void logInvalidLineWidthOnce(const std::string &contentName, World::DrawMode drawMode,
                             double lineWidth) {
    static std::mutex mutex;
    static std::set<std::string> loggedKeys;

    const std::string key = contentName + "|" + std::to_string(static_cast<int>(drawMode));
    std::lock_guard<std::mutex> lock(mutex);
    if (!loggedKeys.insert(key).second) { return; }

    mcvr::log::warn("Entities") << "Skipping line geometry with invalid width " << lineWidth
              << " (content=" << contentName
              << ", drawMode=" << static_cast<int>(drawMode) << ")" << std::endl;
}

constexpr float EMISSIVE_OVERLAY_MATCH_EPSILON = 1.0e-5f;

bool approximatelyEqual(float left, float right) {
    return std::abs(left - right) <= EMISSIVE_OVERLAY_MATCH_EPSILON;
}

bool isEmissiveOverlayBaseCandidate(const std::string &groupName) {
    return groupName != "eyes" &&
           groupName != "entity_translucent_emissive" &&
           groupName != "energy_swirl" &&
           groupName != "beacon_beam" &&
           groupName != "lightning" &&
           groupName != "dragon_rays" &&
           groupName != "priority_outline" &&
           groupName.rfind("priority_outline_", 0) != 0;
}

std::optional<uint32_t> uniformTextureID(
    const std::vector<vk::VertexFormat::PBRVertex> &geometryVertices) {
    if (geometryVertices.empty() || geometryVertices.front().useTexture == 0) { return std::nullopt; }

    const uint32_t textureID = geometryVertices.front().textureID;
    for (const auto &vertex : geometryVertices) {
        if (vertex.useTexture == 0 || vertex.textureID != textureID) { return std::nullopt; }
    }
    return textureID;
}

bool sameTexturedSurface(const std::vector<vk::VertexFormat::PBRVertex> &baseVertices,
                         const std::vector<uint32_t> &baseIndices,
                         const std::vector<vk::VertexFormat::PBRVertex> &overlayVertices,
                         const std::vector<uint32_t> &overlayIndices) {
    if (baseVertices.size() != overlayVertices.size() || baseIndices != overlayIndices) { return false; }

    for (size_t i = 0; i < baseVertices.size(); ++i) {
        const auto &base = baseVertices[i];
        const auto &overlay = overlayVertices[i];
        if (!approximatelyEqual(base.pos.x, overlay.pos.x) ||
            !approximatelyEqual(base.pos.y, overlay.pos.y) ||
            !approximatelyEqual(base.pos.z, overlay.pos.z) ||
            !approximatelyEqual(base.textureUV.x, overlay.textureUV.x) ||
            !approximatelyEqual(base.textureUV.y, overlay.textureUV.y)) {
            return false;
        }
    }
    return true;
}

std::vector<uint32_t> decodeDrawIndices(const EntitiesBuildTask &task, uint32_t geometryIndex,
                                        uint32_t vertexCount, World::DrawMode mode) {
    const int indexCount = task.geometryIndexCounts[geometryIndex];
    if (indexCount < 0) { throw std::runtime_error("Negative world mesh index count"); }
    std::vector<uint32_t> drawIndices(static_cast<size_t>(indexCount));
    if (task.geometryIndices[geometryIndex] == nullptr) {
        drawIndices.clear();
        if (mode == World::DrawMode::QUADS) {
            if (vertexCount % 4 != 0) { throw std::runtime_error("World quad vertex count is not divisible by four"); }
            drawIndices.reserve(vertexCount / 4 * 6);
            for (uint32_t i = 0; i < vertexCount; i += 4) {
                drawIndices.insert(drawIndices.end(), {i, i + 1, i + 2, i + 2, i + 3, i});
            }
        } else {
            drawIndices.reserve(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i) { drawIndices.push_back(i); }
        }
    } else if (task.geometryIndexTypes[geometryIndex] == 0) {
        const auto *source = static_cast<const uint16_t *>(task.geometryIndices[geometryIndex]);
        for (int i = 0; i < indexCount; ++i) { drawIndices[i] = source[i]; }
    } else if (task.geometryIndexTypes[geometryIndex] == 1) {
        const auto *source = static_cast<const uint32_t *>(task.geometryIndices[geometryIndex]);
        std::copy(source, source + indexCount, drawIndices.begin());
    } else {
        throw std::runtime_error("Unsupported world mesh index element type");
    }
    for (uint32_t index : drawIndices) {
        if (index >= vertexCount) { throw std::runtime_error("World mesh index exceeds vertex count"); }
    }
    return drawIndices;
}

size_t worldVertexStride(int format) {
    switch (format) {
        case World::POSITION_COLOR_TEXTURE_LIGHT_NORMAL:
            return sizeof(vk::VertexFormat::PositionColorTexLightNormal);
        case World::POSITION_COLOR_TEXTURE_OVERLAY_LIGHT_NORMAL:
            return sizeof(vk::VertexFormat::PositionColorTexOverlayLightNormal);
        case World::POSITION_TEXTURE_COLOR_LIGHT:
            return sizeof(vk::VertexFormat::PositionTexColorLight);
        case World::POSITION: return sizeof(vk::VertexFormat::PositionOnly);
        case World::POSITION_COLOR: return sizeof(vk::VertexFormat::PositionColor);
        case World::LINES: return sizeof(vk::VertexFormat::PositionColorNormal);
        case World::POSITION_COLOR_LIGHT: return sizeof(vk::VertexFormat::PositionColorLight);
        case World::POSITION_TEXTURE: return sizeof(vk::VertexFormat::PositionTex);
        case World::POSITION_TEXTURE_COLOR: return sizeof(vk::VertexFormat::PositionTexColor);
        case World::POSITION_COLOR_TEXTURE_LIGHT:
            return sizeof(vk::VertexFormat::PositionColorTexLight);
        case World::POSITION_TEXTURE_LIGHT_COLOR:
            return sizeof(vk::VertexFormat::PositionTexLightColor);
        case World::POSITION_TEXTURE_COLOR_NORMAL:
            return sizeof(vk::VertexFormat::PositionTexColorNormal);
        case World::PBR_TRIANGLE: return sizeof(vk::VertexFormat::PBRVertex);
        default: throw std::runtime_error("Unsupported world mesh vertex format");
    }
}

void composeEmissiveEyeOverlays(
    std::vector<uint32_t> &geometryMaterialFlags,
    std::vector<World::GeometryTypes> &geometryTypes,
    std::vector<std::string> &geometryGroupNames,
    std::vector<std::string> &geometryContentNames,
    std::vector<std::vector<vk::VertexFormat::PBRVertex>> &vertices,
    std::vector<std::vector<uint32_t>> &indices,
    std::vector<uint32_t> &emissiveOverlayTextureIDs) {
    emissiveOverlayTextureIDs.assign(vertices.size(), 0u);
    std::vector<size_t> composedOverlayIndices;

    for (size_t overlayIndex = 0; overlayIndex < vertices.size(); ++overlayIndex) {
        if (geometryGroupNames[overlayIndex] != "eyes") { continue; }

        const auto overlayTextureID = uniformTextureID(vertices[overlayIndex]);
        if (!overlayTextureID.has_value() || overlayTextureID.value() == 0u) { continue; }

        std::optional<size_t> baseIndex;
        for (size_t candidate = overlayIndex; candidate-- > 0;) {
            if (!isEmissiveOverlayBaseCandidate(geometryGroupNames[candidate]) ||
                emissiveOverlayTextureIDs[candidate] != 0u) {
                continue;
            }
            if (geometryMaterialFlags[candidate] == geometryMaterialFlags[overlayIndex] &&
                sameTexturedSurface(vertices[candidate], indices[candidate],
                                    vertices[overlayIndex], indices[overlayIndex])) {
                baseIndex = candidate;
                break;
            }
        }
        if (!baseIndex.has_value()) {
            for (size_t candidate = overlayIndex + 1; candidate < vertices.size(); ++candidate) {
                if (!isEmissiveOverlayBaseCandidate(geometryGroupNames[candidate]) ||
                    emissiveOverlayTextureIDs[candidate] != 0u) {
                    continue;
                }
                if (geometryMaterialFlags[candidate] == geometryMaterialFlags[overlayIndex] &&
                sameTexturedSurface(vertices[candidate], indices[candidate],
                                        vertices[overlayIndex], indices[overlayIndex])) {
                    baseIndex = candidate;
                    break;
                }
            }
        }
        if (!baseIndex.has_value()) { continue; }

        emissiveOverlayTextureIDs[baseIndex.value()] = overlayTextureID.value();
        composedOverlayIndices.push_back(overlayIndex);
    }

    for (auto it = composedOverlayIndices.rbegin(); it != composedOverlayIndices.rend(); ++it) {
        const auto overlayIndex = static_cast<std::ptrdiff_t>(*it);
        geometryMaterialFlags.erase(geometryMaterialFlags.begin() + overlayIndex);
        geometryTypes.erase(geometryTypes.begin() + overlayIndex);
        geometryGroupNames.erase(geometryGroupNames.begin() + overlayIndex);
        geometryContentNames.erase(geometryContentNames.begin() + overlayIndex);
        vertices.erase(vertices.begin() + overlayIndex);
        indices.erase(indices.begin() + overlayIndex);
        emissiveOverlayTextureIDs.erase(emissiveOverlayTextureIDs.begin() + overlayIndex);
    }
}

// void logPostContentNameOnce(int postRenderFlag, const std::string &contentName) {
//     static std::mutex mutex;
//     static std::set<std::string> loggedKeys;

//     const std::string flagName = postRenderFlagName(postRenderFlag);
//     const std::string key = flagName + "|" + contentName;

//     std::lock_guard<std::mutex> lock(mutex);
//     if (!loggedKeys.insert(key).second) { return; }

//     mcvr::log::error("Entities") << "[PostContent-Native] flag=" << flagName << " content=" << contentName << std::endl;
// }

}

struct TriangleHash {
    static inline void hash_combine(std::size_t &seed, std::size_t h) noexcept {
        seed ^= h + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    }

    std::size_t operator()(Triangle const &k) const noexcept {
        std::size_t seed = 0;
        for (auto const &v : k) { hash_combine(seed, std::hash<glm::vec3>{}(v)); }
        return seed;
    }
};

static void buildEntityPackedVertices(const std::vector<std::vector<vk::VertexFormat::PBRVertex>> &vertices,
                                      const std::vector<std::vector<uint32_t>> &indices,
                                      const std::vector<uint32_t> &emissiveOverlayTextureIDs,
                                      std::vector<vk::VertexFormat::PositionVertex> &packedPositions,
                                      std::vector<vk::VertexFormat::MaterialVertex> &packedMaterials,
                                      std::vector<uint32_t> &packedIndices) {
    for (int i = 0; i < static_cast<int>(vertices.size()); i++) {
        const auto &geometryVertices = vertices[i];
        const auto &geometryIndices = indices[i];

        packedIndices.insert(packedIndices.end(), geometryIndices.begin(), geometryIndices.end());

        vk::Vertex::appendPackedVertices(geometryVertices, emissiveOverlayTextureIDs[i],
                                         packedPositions, packedMaterials);
    }
}


EntityBuildData::EntityBuildData(int hashCode,
                                 double x,
                                 double y,
                                 double z,
                                 int rayTracingFlag,
                                 int postRenderFlag,
                                 int prebuiltBLAS,
                                 World::Coordinates coordinate,
                                 uint32_t geometryCount,
                                 std::vector<World::GeometryTypes> &&geometryTypes,
                                 std::vector<std::string> &&geometryGroupNames,
                                 std::vector<std::string> &&geometryContentNames,
                                 std::vector<std::vector<vk::VertexFormat::PBRVertex>> &&vertices,
                                 std::vector<std::vector<uint32_t>> &&indices,
                                 std::vector<uint32_t> &&emissiveOverlayTextureIDs,
                                 uint64_t worldToken,
                                 uint64_t frameToken,
                                 uint64_t resourceGeneration,
                                 uint64_t stageToken,
                                 int worldStage,
                                 std::vector<std::string> &&shaderKeys,
                                 std::vector<std::string> &&materialKeys)
    : geometryMaterialFlags(geometryCount, 0), hashCode(hashCode),
      x(x),
      y(y),
      z(z),
      rayTracingFlag(rayTracingFlag),
      postRenderFlag(postRenderFlag),
      prebuiltBLAS(prebuiltBLAS),
      coordinate(coordinate),
      geometryCount(geometryCount),
      geometryTypes(std::move(geometryTypes)),
      geometryGroupNames(std::move(geometryGroupNames)),
      geometryContentNames(std::move(geometryContentNames)),
      vertices(std::move(vertices)),
      indices(std::move(indices)),
      emissiveOverlayTextureIDs(std::move(emissiveOverlayTextureIDs)),
      worldToken(worldToken),
      frameToken(frameToken),
      resourceGeneration(resourceGeneration),
      stageToken(stageToken),
      worldStage(worldStage),
      shaderKeys(std::move(shaderKeys)),
      materialKeys(std::move(materialKeys)),
      indexBufferAddresses(),
      positionBufferAddresses(),
      materialBufferAddresses() {}

void EntityBuildDataBatch::addData(std::shared_ptr<EntityBuildData> data) {
    datas.push_back(data);
}

void EntityBuildDataBatch::build() {
    auto clearBatch = [this]() {
        datas.clear();
        indexBuffer = nullptr;
        positionBuffer = nullptr;
        materialBuffer = nullptr;
        blasBatchBuilder = nullptr;
    };

    if (datas.empty()) {
        clearBatch();
        return;
    }

    auto framework = Renderer::instance().framework();
    auto vma = framework->vma();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();

    std::vector<uint32_t> instanceOffsets;
    std::vector<uint32_t> geometryVertexOffsets;
    std::vector<uint32_t> geometryIndexOffsets;
    uint32_t totalGeometryCount = 0;
    uint32_t totalVertexCount = 0;
    uint32_t totalIndexCount = 0;

    for (auto data : datas) {
        instanceOffsets.push_back(totalGeometryCount);
        for (int i = 0; i < data->geometryCount; i++) {
            geometryVertexOffsets.push_back(totalVertexCount);
            geometryIndexOffsets.push_back(totalIndexCount);

            totalVertexCount += data->vertices[i].size();
            totalIndexCount += data->indices[i].size();
        }

        totalGeometryCount += data->geometryCount;
    }

    if (totalGeometryCount == 0 || totalVertexCount == 0 || totalIndexCount == 0) {
        clearBatch();
        return;
    }

    positionBuffer = vk::DeviceLocalBuffer::create(
        vma, device, false, totalVertexCount * sizeof(vk::VertexFormat::PositionVertex),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    materialBuffer = vk::DeviceLocalBuffer::create(
        vma, device, false, totalVertexCount * sizeof(vk::VertexFormat::MaterialVertex),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    indexBuffer = vk::DeviceLocalBuffer::create(
        vma, device, false, totalIndexCount * sizeof(uint32_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    std::vector<vk::VertexFormat::PositionVertex> packedPositions;
    std::vector<vk::VertexFormat::MaterialVertex> packedMaterials;
    std::vector<uint32_t> packedIndices;
    packedPositions.reserve(totalVertexCount);
    packedMaterials.reserve(totalVertexCount);
    packedIndices.reserve(totalIndexCount);
    for (auto data : datas) {
        buildEntityPackedVertices(data->vertices, data->indices, data->emissiveOverlayTextureIDs,
                                  packedPositions, packedMaterials, packedIndices);
    }

    if (!packedPositions.empty()) {
        positionBuffer->uploadToStagingBuffer(packedPositions.data(),
                                              packedPositions.size() * sizeof(vk::VertexFormat::PositionVertex), 0);
        materialBuffer->uploadToStagingBuffer(packedMaterials.data(),
                                              packedMaterials.size() * sizeof(vk::VertexFormat::MaterialVertex), 0);
    }
    if (!packedIndices.empty()) {
        indexBuffer->uploadToStagingBuffer(packedIndices.data(), packedIndices.size() * sizeof(uint32_t), 0);
    }
    positionBuffer->flushStagingBuffer();
    materialBuffer->flushStagingBuffer();
    indexBuffer->flushStagingBuffer();

    blasBatchBuilder = vk::BLASBatchBuilder::create();
    std::vector<uint32_t> nonPrebuildInstances;
    for (int instanceIndex = 0; auto data : datas) {
        auto instanceOffset = instanceOffsets[instanceIndex];
        std::shared_ptr<vk::BLASBuilder> blasBuilder = nullptr;
        std::shared_ptr<vk::BLASBuilder::BLASGeometryBuilder> blasGeometryBuilder = nullptr;
        if (data->prebuiltBLAS < 0) {
            nonPrebuildInstances.push_back(instanceIndex);
            blasBuilder = blasBatchBuilder->defineBLASBuilder();
            blasGeometryBuilder = blasBuilder->beginGeometries();
        }
        for (int i = 0; i < data->geometryCount; i++) {
            VkDeviceAddress indexBufferAddress =
                indexBuffer->bufferAddress() + geometryIndexOffsets[instanceOffset + i] * sizeof(uint32_t);
            VkDeviceAddress positionBufferAddress =
                positionBuffer->bufferAddress() +
                geometryVertexOffsets[instanceOffset + i] * sizeof(vk::VertexFormat::PositionVertex);
            VkDeviceAddress materialBufferAddress =
                materialBuffer->bufferAddress() +
                geometryVertexOffsets[instanceOffset + i] * sizeof(vk::VertexFormat::MaterialVertex);
            data->indexBufferAddresses.push_back(indexBufferAddress);
            data->positionBufferAddresses.push_back(positionBufferAddress);
            data->materialBufferAddresses.push_back(materialBufferAddress);
            if (data->prebuiltBLAS < 0) {
                blasGeometryBuilder->defineTriangleGeomrtry<vk::VertexFormat::PositionVertex>(
                    positionBufferAddress, data->vertices[i].size(), indexBufferAddress, data->indices[i].size(),
                    data->geometryTypes[i] == World::WORLD_SOLID &&
                    !mcvr::faces::needsAnyHit(data->geometryMaterialFlags[i], data->geometryMaterialFlags));
            }
        }
        if (data->prebuiltBLAS < 0) {
            blasGeometryBuilder->endGeometries();
            blasBuilder->defineBuildProperty(VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR)
                ->querySizeInfo(device);
        }

        instanceIndex++;
    }

    auto blass = blasBatchBuilder->allocateBuffers(physicalDevice, device, vma)->build(device);
    for (int i = 0; i < nonPrebuildInstances.size(); i++) { datas[nonPrebuildInstances[i]]->blas = blass[i]; }
}

void EntityPostBuildDataBatch::addData(std::shared_ptr<EntityBuildData> data) {
    datas.push_back(data);
}

Entity::Entity(std::shared_ptr<EntityBuildData> chunkBuildData) {
    geometryMaterialFlags = chunkBuildData->geometryMaterialFlags;
    hashCode = chunkBuildData->hashCode;
    x = chunkBuildData->x;
    y = chunkBuildData->y;
    z = chunkBuildData->z;
    rayTracingFlag = chunkBuildData->rayTracingFlag;
    prebuiltBLAS = chunkBuildData->prebuiltBLAS;
    coordinate = chunkBuildData->coordinate;
    worldToken = chunkBuildData->worldToken;
    frameToken = chunkBuildData->frameToken;
    resourceGeneration = chunkBuildData->resourceGeneration;
    stageToken = chunkBuildData->stageToken;
    worldStage = chunkBuildData->worldStage;
    shaderKeys = std::make_shared<std::vector<std::string>>(std::move(chunkBuildData->shaderKeys));
    materialKeys = std::make_shared<std::vector<std::string>>(std::move(chunkBuildData->materialKeys));

    blas = chunkBuildData->blas;
    indexBufferAddresses =
        std::make_shared<std::vector<VkDeviceAddress>>(std::move(chunkBuildData->indexBufferAddresses));
    positionBufferAddresses =
        std::make_shared<std::vector<VkDeviceAddress>>(std::move(chunkBuildData->positionBufferAddresses));
    materialBufferAddresses =
        std::make_shared<std::vector<VkDeviceAddress>>(std::move(chunkBuildData->materialBufferAddresses));

    geometryCount = chunkBuildData->geometryCount;
    geometryGroupNames = std::make_shared<std::vector<std::string>>(std::move(chunkBuildData->geometryGroupNames));
    geometryContentNames =
        std::make_shared<std::vector<std::string>>(std::move(chunkBuildData->geometryContentNames));
    vertexCounts = std::make_shared<std::vector<uint32_t>>();
    indexCounts = std::make_shared<std::vector<uint32_t>>();
    vertexCounts->reserve(geometryCount);
    indexCounts->reserve(geometryCount);
    for (uint32_t i = 0; i < geometryCount; i++) {
        vertexCounts->push_back(static_cast<uint32_t>(chunkBuildData->vertices[i].size()));
        indexCounts->push_back(static_cast<uint32_t>(chunkBuildData->indices[i].size()));
    }
}

EntityBatch::EntityBatch(std::shared_ptr<EntityBuildDataBatch> entityBuildDataBatch) {
    for (auto data : entityBuildDataBatch->datas) {
        auto entity = Entity::create(data);
        entity->indexBuffer = entityBuildDataBatch->indexBuffer;
        entity->positionBuffer = entityBuildDataBatch->positionBuffer;
        entity->materialBuffer = entityBuildDataBatch->materialBuffer;
        entities.push_back(entity);
    }

    indexBuffer = entityBuildDataBatch->indexBuffer;
    positionBuffer = entityBuildDataBatch->positionBuffer;
    materialBuffer = entityBuildDataBatch->materialBuffer;
}

EntityPost::EntityPost(std::shared_ptr<EntityBuildData> chunkBuildData) {
    postRenderFlag = chunkBuildData->postRenderFlag;
    x = chunkBuildData->x;
    y = chunkBuildData->y;
    z = chunkBuildData->z;

    geometryCount = chunkBuildData->geometryCount;
    geometryContentNames = std::move(chunkBuildData->geometryContentNames);
    indexCounts.reserve(geometryCount);

    auto framework = Renderer::instance().framework();
    auto vma = framework->vma();
    auto device = framework->device();

    for (int i = 0; i < geometryCount; i++) {
        auto vertexBuffer = vk::DeviceLocalBuffer::create(
            vma, device, false, chunkBuildData->vertices[i].size() * sizeof(vk::VertexFormat::PBRVertex),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        auto indexBuffer = vk::DeviceLocalBuffer::create(vma, device, false,
                                                         chunkBuildData->indices[i].size() * sizeof(uint32_t),
                                                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        vertexBuffer->uploadToStagingBuffer(chunkBuildData->vertices[i].data());
        indexBuffer->uploadToStagingBuffer(chunkBuildData->indices[i].data());

        vertexBuffers.push_back(vertexBuffer);
        indexBuffers.push_back(indexBuffer);
        indexCounts.push_back(static_cast<uint32_t>(chunkBuildData->indices[i].size()));
    }
}

EntityPostBatch::EntityPostBatch(std::shared_ptr<EntityPostBuildDataBatch> entityPostBuildDataBatch) {
    for (auto data : entityPostBuildDataBatch->datas) { entities.push_back(EntityPost::create(data)); }
}

Entities::Entities(std::shared_ptr<Framework> framework) {}

bool Entities::beginCachedCloud(uint64_t revision, double x, double y, double z) {
    if (revision == 0 || cloudCapturing_ || !entityBuildDataBatch_)
        throw std::logic_error("Invalid cloud geometry capture boundary");
    if (auto cached = cloudCache_.find(revision)) {
        for (const auto &entity : cached->entities) {
            // Per-frame transforms must not mutate entities retained by previous TLAS/history.
            auto next = std::make_shared<Entity>(*entity);
            next->x = x; next->y = y; next->z = z;
            queuedClouds_.push_back(std::move(next));
        }
        return true;
    }
    cloudRevision_ = revision;
    cloudCaptureStart_ = entityBuildDataBatch_->datas.size();
    cloudCapturing_ = true;
    return false;
}

void Entities::endCachedCloud(bool success) {
    if (!cloudCapturing_) throw std::logic_error("Cloud capture was not opened");
    cloudCapturing_ = false;
    auto &datas = entityBuildDataBatch_->datas;
    if (success) {
        cloudBuildData_ = EntityBuildDataBatch::create();
        cloudBuildData_->datas.assign(datas.begin() + cloudCaptureStart_, datas.end());
    }
    datas.erase(datas.begin() + cloudCaptureStart_, datas.end());
}

void Entities::recordCachedCloudBuild(const std::shared_ptr<vk::CommandBuffer> &commands) {
    if (cloudBlasBuilder_ && !cloudBuildRecorded_) {
        cloudBlasBuilder_->submit(commands);
        cloudBuildRecorded_ = true;
    }
}

void Entities::commitCachedCloudBuild() {
    if (cloudBuildRecorded_) cloudCache_.submitted();
}

void Entities::publishCachedEntities(std::vector<std::shared_ptr<Entity>> entities) {
    auto &retainer = Renderer::instance().framework()->frameResourceRetainer();
    retainer.retain(entityBatch_);
    retainer.retain(blasBatchBuilder_);
    blasBatchBuilder_.reset();
    entityBatch_ = EntityBatch::create();
    entityBatch_->entities = std::move(entities);
}

std::shared_ptr<Entity> Entities::buildUiMaterialUpdate(const std::shared_ptr<Entity> &previous) {
    if (!previous || entityBuildDataBatch_->datas.size() != 1) return {};
    const auto &data = entityBuildDataBatch_->datas.front();
    if (data->geometryCount != previous->geometryCount) return {};
    std::vector<vk::VertexFormat::MaterialVertex> materials;
    std::vector<size_t> offsets;
    for (uint32_t i = 0; i < data->geometryCount; ++i) {
        if (data->vertices[i].size() != previous->vertexCounts->at(i) ||
            data->indices[i].size() != previous->indexCounts->at(i) ||
            data->geometryGroupNames[i] != previous->geometryGroupNames->at(i)) return {};
        offsets.push_back(materials.size());
        auto part = vk::Vertex::buildMaterialVertices(data->vertices[i]);
        for (auto &vertex : part) vertex.emissiveOverlayTextureID = data->emissiveOverlayTextureIDs[i];
        materials.insert(materials.end(), part.begin(), part.end());
    }
    if (materials.empty()) return {};
    auto framework = Renderer::instance().framework();
    auto next = std::make_shared<Entity>(*previous);
    next->materialBuffer = vk::DeviceLocalBuffer::create(framework->vma(), framework->device(), false,
        materials.size() * sizeof(materials[0]), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    next->materialBuffer->uploadToStagingBuffer(materials.data());
    next->materialBufferAddresses = std::make_shared<std::vector<VkDeviceAddress>>();
    for (auto offset : offsets)
        next->materialBufferAddresses->push_back(next->materialBuffer->bufferAddress() + offset * sizeof(materials[0]));
    Renderer::instance().buffers()->queueImportantWorldUpload(next->materialBuffer);
    framework->frameResourceRetainer().retain(previous);
    return next;
}

void Entities::resetFrame() {
    auto framework = Renderer::instance().framework();
    framework->safeAcquireCurrentContext();
    auto &frr = framework->frameResourceRetainer();
    frr.retain(cloudBuildData_);
    frr.retain(cloudBlasBuilder_);
    cloudBuildData_.reset();
    cloudBlasBuilder_.reset();
    queuedClouds_.clear();
    cloudCapturing_ = cloudBuildRecorded_ = false;
    cloudCache_.beginFrame();

    frr.retain(entityBuildDataBatch_);
    entityBuildDataBatch_ = EntityBuildDataBatch::create();

    frr.retain(entityPostBuildDataBatch_);
    entityPostBuildDataBatch_ = EntityPostBuildDataBatch::create();

    frr.retain(entityBatch_);
    entityBatch_ = nullptr;

    frr.retain(entityPostBatch_);
    entityPostBatch_ = nullptr;

    frr.retain(blasBatchBuilder_);
    blasBatchBuilder_ = nullptr;
}

int Entities::beginWorldMeshFrame(uint64_t worldToken, uint64_t frameToken,
                                  uint64_t resourceGeneration) {
    if (worldToken == 0 || frameToken == 0 || resourceGeneration == 0) return 3;
    if (entityBuildDataBatch_ == nullptr) entityBuildDataBatch_ = EntityBuildDataBatch::create();
    if (entityPostBuildDataBatch_ == nullptr) entityPostBuildDataBatch_ = EntityPostBuildDataBatch::create();
    auto removeWorldMeshes = [](auto &datas) {
        std::erase_if(datas, [](const auto &data) { return data != nullptr && data->worldToken != 0; });
    };
    removeWorldMeshes(entityBuildDataBatch_->datas);
    removeWorldMeshes(entityPostBuildDataBatch_->datas);
    activeWorldToken_ = worldToken;
    activeFrameToken_ = frameToken;
    activeResourceGeneration_ = resourceGeneration;
    frameEntityCheckpoint_ = entityBuildDataBatch_->datas.size();
    framePostCheckpoint_ = entityPostBuildDataBatch_->datas.size();
    worldMeshStages_.clear();
    worldMeshFrameOpen_ = true;
    return 1;
}

int Entities::beginWorldMeshStage(uint64_t worldToken, uint64_t frameToken,
                                  uint64_t resourceGeneration, uint64_t stageToken, int) {
    if (!worldMeshFrameOpen_ || !acceptsWorldMeshGeneration(worldToken, frameToken,
                                                            resourceGeneration)) return 2;
    if (stageToken == 0) return 3;
    worldMeshStages_.push_back({stageToken, entityBuildDataBatch_->datas.size(),
                               entityPostBuildDataBatch_->datas.size()});
    return 1;
}

int Entities::queueWorldMesh(const WorldMeshBuildTask &task) {
    if (!worldMeshFrameOpen_ || !acceptsWorldMeshGeneration(
            task.worldToken, task.frameToken, task.resourceGeneration) ||
        worldMeshStages_.empty() || worldMeshStages_.back().token != task.stageToken) return 2;
    if (task.vertices == nullptr || task.vertexCount <= 0 || task.vertexBytes <= 0 ||
        task.indexCount <= 0 || task.indexBytes < 0 ||
        (task.indices == nullptr && task.indexBytes != 0)) return 3;
    const size_t expectedVertexBytes = worldVertexStride(task.vertexFormat) *
                                       static_cast<size_t>(task.vertexCount);
    if (expectedVertexBytes != static_cast<size_t>(task.vertexBytes)) return 3;
    const size_t indexStride = task.indexType == 0 ? sizeof(uint16_t) :
                               task.indexType == 1 ? sizeof(uint32_t) : 0;
    if (indexStride == 0 || (task.indices != nullptr &&
        indexStride * static_cast<size_t>(task.indexCount) != static_cast<size_t>(task.indexBytes))) return 3;
    if (task.drawMode < static_cast<int>(World::DrawMode::TRIANGLES) ||
        task.drawMode > static_cast<int>(World::DrawMode::QUADS)) return 3;
    if (task.coordinate < World::Coordinates::WORLD ||
        task.coordinate > World::Coordinates::CAMERA_SHIFT ||
        (task.geometryType & 0xff) < World::GeometryTypes::SHADOW ||
        (task.geometryType & 0xff) >= World::GeometryTypes::NUM_GEOMETRY_TYPES ||
        task.alphaMode < 0 || task.alphaMode > 24 || !std::isfinite(task.emission) ||
        task.emission < 0.0f || task.shaderKey == nullptr || task.materialKey == nullptr) return 3;

    int entityHashCode = task.sourceId;
    double entityX = task.originX, entityY = task.originY, entityZ = task.originZ;
    int rayTracingFlag = task.rayTracingFlag;
    int postRenderFlag = 0, prebuiltBlas = -1, post = 0, geometryCount = 1;
    int geometryType = task.geometryType, texture = task.textureId;
    int vertexFormat = task.vertexFormat, drawMode = task.drawMode, vertexCount = task.vertexCount;
    int indexType = task.indexType, indexCount = task.indexCount, indexBytes = task.indexBytes;
    int alphaMode = task.alphaMode;
    float emission = task.emission;
    void *vertices = task.vertices, *indices = task.indices;
    const char *materialKey = task.materialKey == nullptr ? "" : task.materialKey;
    std::string groupName(materialKey);
    if (const auto separator = groupName.find('|'); separator != std::string::npos) {
        groupName.erase(separator);
    }
    if (groupName.empty()) groupName = "world_stage";
    const char *groupNamePtr = groupName.c_str();
    const char *contentNamePtr = materialKey;
    const char *shaderKey = task.shaderKey == nullptr ? "" : task.shaderKey;

    const size_t before = entityBuildDataBatch_->datas.size();
    queueBuild(EntitiesBuildTask{
        .lineWidth = 0.0125f,
        .coordinate = task.coordinate,
        .normalOffset = false,
        .entityCount = 1,
        .entityHashCodes = &entityHashCode,
        .entityXs = &entityX,
        .entityYs = &entityY,
        .entityZs = &entityZ,
        .entityRayTracingFlags = &rayTracingFlag,
        .entityPostRenderFlags = &postRenderFlag,
        .entityPrebuiltBLASs = &prebuiltBlas,
        .entityPosts = &post,
        .entityGeometryCounts = &geometryCount,
        .geometryTypes = &geometryType,
        .geometryGroupNames = &groupNamePtr,
        .geometryContentNames = &contentNamePtr,
        .geometryTextures = &texture,
        .vertexFormats = &vertexFormat,
        .indexFormats = &drawMode,
        .vertexCounts = &vertexCount,
        .vertices = &vertices,
        .worldToken = task.worldToken,
        .frameToken = task.frameToken,
        .resourceGeneration = task.resourceGeneration,
        .stageToken = task.stageToken,
        .stage = task.stage,
        .geometryIndexTypes = &indexType,
        .geometryIndexCounts = &indexCount,
        .geometryIndexByteCounts = &indexBytes,
        .geometryIndices = &indices,
        .geometryAlphaModes = &alphaMode,
        .geometryEmissions = &emission,
        .geometryShaderKeys = &shaderKey,
        .geometryMaterialKeys = &materialKey,
    });
    if (entityBuildDataBatch_->datas.size() <= before) return 3;
    if (task.auditId != 0) {
        for (size_t i = before; i < entityBuildDataBatch_->datas.size(); ++i) {
            if (entityBuildDataBatch_->datas[i] != nullptr) {
                entityBuildDataBatch_->datas[i]->auditId = task.auditId;
            }
        }
        worldMeshAuditStates_[task.auditId] = WorldMeshAuditState::Queued;
    }
    return 1;
}

int Entities::pollWorldMeshAudit(uint64_t auditId, bool consumeTerminal) {
    const auto found = worldMeshAuditStates_.find(auditId);
    if (found == worldMeshAuditStates_.end()) return 0;
    const auto status = found->second;
    if (consumeTerminal && status >= WorldMeshAuditState::Built) {
        worldMeshAuditStates_.erase(found);
    }
    return static_cast<int>(status);
}

void Entities::endWorldMeshStage(uint64_t worldToken, uint64_t frameToken,
                                 uint64_t resourceGeneration, uint64_t stageToken, bool commit) {
    if (!acceptsWorldMeshGeneration(worldToken, frameToken, resourceGeneration) ||
        worldMeshStages_.empty() || worldMeshStages_.back().token != stageToken) {
        throw std::runtime_error("World mesh stage closed out of order or after generation change");
    }
    const StageCheckpoint checkpoint = worldMeshStages_.back();
    worldMeshStages_.pop_back();
    if (!commit) {
        for (size_t i = checkpoint.entityCount; i < entityBuildDataBatch_->datas.size(); ++i) {
            const auto &data = entityBuildDataBatch_->datas[i];
            if (data != nullptr && data->auditId != 0) {
                worldMeshAuditStates_[data->auditId] = WorldMeshAuditState::RolledBack;
            }
        }
        entityBuildDataBatch_->datas.resize(checkpoint.entityCount);
        entityPostBuildDataBatch_->datas.resize(checkpoint.postCount);
    }
}

void Entities::endWorldMeshFrame(uint64_t worldToken, uint64_t frameToken,
                                 uint64_t resourceGeneration, bool commit) {
    if (!acceptsWorldMeshGeneration(worldToken, frameToken, resourceGeneration)) {
        throw std::runtime_error("World mesh frame closed after generation change");
    }
    if (!worldMeshStages_.empty()) throw std::runtime_error("World mesh frame has active stages");
    if (!commit) {
        for (size_t i = frameEntityCheckpoint_; i < entityBuildDataBatch_->datas.size(); ++i) {
            const auto &data = entityBuildDataBatch_->datas[i];
            if (data != nullptr && data->auditId != 0) {
                worldMeshAuditStates_[data->auditId] = WorldMeshAuditState::RolledBack;
            }
        }
        entityBuildDataBatch_->datas.resize(frameEntityCheckpoint_);
        entityPostBuildDataBatch_->datas.resize(framePostCheckpoint_);
    } else {
        for (size_t i = frameEntityCheckpoint_; i < entityBuildDataBatch_->datas.size(); ++i) {
            const auto &data = entityBuildDataBatch_->datas[i];
            if (data != nullptr && data->auditId != 0) {
                worldMeshAuditStates_[data->auditId] = WorldMeshAuditState::Committed;
            }
        }
    }
    worldMeshFrameOpen_ = false;
}

void Entities::invalidateWorldMeshGeneration(uint64_t resourceGeneration) {
    activeResourceGeneration_ = resourceGeneration;
    worldMeshFrameOpen_ = false;
    worldMeshStages_.clear();
    if (entityBuildDataBatch_ != nullptr) {
        for (const auto &data : entityBuildDataBatch_->datas) {
            if (data != nullptr && data->worldToken != 0 && data->auditId != 0) {
                worldMeshAuditStates_[data->auditId] = WorldMeshAuditState::Stale;
            }
        }
        std::erase_if(entityBuildDataBatch_->datas,
                      [](const auto &data) { return data != nullptr && data->worldToken != 0; });
    }
    if (entityPostBuildDataBatch_ != nullptr) {
        std::erase_if(entityPostBuildDataBatch_->datas,
                      [](const auto &data) { return data != nullptr && data->worldToken != 0; });
    }
}

bool Entities::acceptsWorldMeshGeneration(uint64_t worldToken, uint64_t frameToken,
                                          uint64_t resourceGeneration) const {
    return WorldMeshContract::generationMatches(activeWorldToken_, activeFrameToken_,
        activeResourceGeneration_, worldToken, frameToken, resourceGeneration);
}

void Entities::queueBuild(EntitiesBuildTask task) {
    Renderer::instance().framework()->safeAcquireCurrentContext();
    auto framework = Renderer::instance().framework();
    auto vma = framework->vma();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();

    std::set<int> textureIDs;

    uint32_t geometryAccu = 0;
    for (int e = 0; e < task.entityCount; e++) {
        uint32_t geometryIndex = geometryAccu;
        uint32_t geometryCountIncludeGlint = task.entityGeometryCounts[e];
        geometryAccu += geometryCountIncludeGlint;

        uint32_t allVertexCount = 0, allIndexCount = 0;
        std::vector<uint32_t> geometryMaterialFlags;
        std::vector<World::GeometryTypes> geometryTypes;
        std::vector<std::string> geometryGroupNames;
        std::vector<std::string> geometryContentNames;
        std::vector<std::vector<vk::VertexFormat::PBRVertex>> vertices;
        std::vector<std::vector<uint32_t>> indices;
        std::vector<uint32_t> emissiveOverlayTextureIDs;
        std::vector<std::string> shaderKeys;
        std::vector<std::string> materialKeys;
        int hashCode = task.entityHashCodes[e];
        double x = task.entityXs[e];
        double y = task.entityYs[e];
        double z = task.entityZs[e];
        int rayTracingFlag = task.entityRayTracingFlags[e];
        int postRenderFlag = task.entityPostRenderFlags[e];
        int prebuiltBLAS = task.entityPrebuiltBLASs[e];
        World::Coordinates coordinate = task.coordinate;
        bool post = task.entityPosts[e];

        // Line-extrusion roll frame. Identity for ordinary world geometry; rotated owners pass
        // their own orthonormal axes so the square section keeps a stable orientation.
        glm::dvec3 lineFrameX{1.0, 0.0, 0.0};
        glm::dvec3 lineFrameY{0.0, 1.0, 0.0};
        glm::dvec3 lineFrameZ{0.0, 0.0, 1.0};
        if (task.entityLineFrames != nullptr) {
            const float *frame = task.entityLineFrames + static_cast<size_t>(e) * 9u;
            lineFrameX = glm::dvec3(frame[0], frame[1], frame[2]);
            lineFrameY = glm::dvec3(frame[3], frame[4], frame[5]);
            lineFrameZ = glm::dvec3(frame[6], frame[7], frame[8]);
        }

        uint32_t geometryCountWithoutGlint = 0;
        for (int i = 0; i < task.entityGeometryCounts[e]; i++) {
            World::GeometryTypes geometryType =
                static_cast<World::GeometryTypes>(task.geometryTypes[geometryIndex + i] & 0xff);
            geometryMaterialFlags.push_back(static_cast<uint32_t>(task.geometryTypes[geometryIndex + i]) >> 8u);
            int geometryTexture = task.geometryTextures[geometryIndex + i];
            geometryTypes.push_back(geometryType);
            if (task.geometryGroupNames != nullptr && task.geometryGroupNames[geometryIndex + i] != nullptr) {
                geometryGroupNames.emplace_back(task.geometryGroupNames[geometryIndex + i]);
            } else {
                geometryGroupNames.emplace_back("Entity");
            }
            if (task.geometryContentNames != nullptr && task.geometryContentNames[geometryIndex + i] != nullptr) {
                geometryContentNames.emplace_back(task.geometryContentNames[geometryIndex + i]);
            } else {
                geometryContentNames.emplace_back("");
            }
            shaderKeys.emplace_back(task.geometryShaderKeys != nullptr &&
                                            task.geometryShaderKeys[geometryIndex + i] != nullptr
                                        ? task.geometryShaderKeys[geometryIndex + i]
                                        : "");
            materialKeys.emplace_back(task.geometryMaterialKeys != nullptr &&
                                              task.geometryMaterialKeys[geometryIndex + i] != nullptr
                                          ? task.geometryMaterialKeys[geometryIndex + i]
                                          : "");
            // Every world debug/overlay line is true emissive geometry (unified emission = 1.0),
            // including the block selection outline.
            const bool emissiveDebug =
                geometryContentNames.back().rfind("radiance:debug/line", 0) == 0;
            // Model outline edges carry a pose-local reference axis in their normal so the native
            // square-section extrusion follows each part's own frame (including a sub-level pose)
            // instead of the coarse per-entity frame.
            const bool outlineFrame =
                geometryContentNames.back().rfind("radiance:priority/outline", 0) == 0;
            const auto &geometryGroupName = geometryGroupNames.back();
            const bool nameTagContent =
                geometryContentNames.back().rfind("/name_tag/", 0) == 0;
            const bool nameTagBackground =
                geometryGroupName.find("text_background") != std::string::npos;
            const bool semanticEmission =
                (nameTagContent && !nameTagBackground) ||
                geometryGroupName == "priority_outline" ||
                geometryGroupName.rfind("priority_outline_", 0) == 0 ||
                geometryGroupName == "entity_translucent_emissive" ||
                geometryGroupName == "eyes" ||
                geometryGroupName == "entity_alpha" ||
                geometryGroupName == "energy_swirl" ||
                geometryGroupName == "beacon_beam" ||
                geometryGroupName == "lightning" ||
                geometryGroupName == "dragon_rays";
            // if (post && postRenderFlag != 0) {
            //     logPostContentNameOnce(postRenderFlag, geometryContentNames.back());
            // }

            auto &geometryVertices = vertices.emplace_back();
            auto &geometryIndices = indices.emplace_back();

            if (task.vertexFormats[geometryIndex + i] == World::PBR_TRIANGLE) {
                geometryVertices.resize(task.vertexCounts[geometryIndex + i]);
                std::memcpy(geometryVertices.data(), task.vertices[geometryIndex + i],
                            task.vertexCounts[geometryIndex + i] * sizeof(vk::VertexFormat::PBRVertex));
            } else {
                for (int j = 0; j < task.vertexCounts[geometryIndex + i]; j++) {
                    vk::VertexFormat::PBRVertex vertex{};

                    switch (task.vertexFormats[geometryIndex + i]) {
                        case World::POSITION_COLOR_TEXTURE_LIGHT_NORMAL: {
                            vk::VertexFormat::PositionColorTexLightNormal *vertices =
                                static_cast<vk::VertexFormat::PositionColorTexLightNormal *>(
                                    task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            vertex.useColorLayer = 1;
                            vertex.colorLayer = glm::vec4{
                                vertices[j].color & 0xFF,
                                (vertices[j].color >> 8) & 0xFF,
                                (vertices[j].color >> 16) & 0xFF,
                                (vertices[j].color >> 24) & 0xFF,
                            };
                            vertex.colorLayer /= 255.0;

                            vertex.useTexture = 1;
                            vertex.textureUV = vertices[j].uv0;

                            vertex.useLight = 1;
                            vertex.lightUV = glm::ivec2{
                                vertices[j].uv2 & 0xFFFF,
                                (vertices[j].uv2 >> 16) & 0xFFFF,
                            };

                            vertex.useNorm = 1;
                            vertex.norm = glm::vec3{
                                (int8_t)(vertices[j].normal & 0xFF),
                                (int8_t)((vertices[j].normal >> 8) & 0xFF),
                                (int8_t)((vertices[j].normal >> 16) & 0xFF),
                            };

                            break;
                        }
                        case World::POSITION_COLOR_TEXTURE_OVERLAY_LIGHT_NORMAL: {
                            vk::VertexFormat::PositionColorTexOverlayLightNormal *vertices =
                                static_cast<vk::VertexFormat::PositionColorTexOverlayLightNormal *>(
                                    task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            vertex.useColorLayer = 1;
                            vertex.colorLayer = glm::vec4{
                                vertices[j].color & 0xFF,
                                (vertices[j].color >> 8) & 0xFF,
                                (vertices[j].color >> 16) & 0xFF,
                                (vertices[j].color >> 24) & 0xFF,
                            };
                            vertex.colorLayer /= 255.0;

                            vertex.useTexture = 1;
                            vertex.textureUV = vertices[j].uv0;

                            vertex.useOverlay = 1;
                            vertex.overlayUV = glm::ivec2{vertices[j].uv1 & 0xFFFF, (vertices[j].uv1 >> 16) & 0xFFFF};

                            vertex.useLight = 1;
                            vertex.lightUV = glm::vec2{
                                vertices[j].uv2 & 0xFFFF,
                                (vertices[j].uv2 >> 16) & 0xFFFF,
                            };

                            vertex.useNorm = 1;
                            vertex.norm = glm::vec3{
                                (int8_t)(vertices[j].normal & 0xFF),
                                (int8_t)((vertices[j].normal >> 8) & 0xFF),
                                (int8_t)((vertices[j].normal >> 16) & 0xFF),
                            };

                            break;
                        }
                        case World::POSITION_TEXTURE_COLOR_LIGHT: {
                            vk::VertexFormat::PositionTexColorLight *vertices =
                                static_cast<vk::VertexFormat::PositionTexColorLight *>(
                                    task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            vertex.useTexture = 1;
                            vertex.textureUV = vertices[j].uv0;

                            vertex.useColorLayer = 1;
                            vertex.colorLayer = glm::vec4{
                                vertices[j].color & 0xFF,
                                (vertices[j].color >> 8) & 0xFF,
                                (vertices[j].color >> 16) & 0xFF,
                                (vertices[j].color >> 24) & 0xFF,
                            };
                            vertex.colorLayer /= 255.0;

                            vertex.useLight = 1;
                            vertex.lightUV = glm::vec2{
                                vertices[j].uv2 & 0xFFFF,
                                (vertices[j].uv2 >> 16) & 0xFFFF,
                            };

                            break;
                        }
                        case World::POSITION: {
                            vk::VertexFormat::PositionOnly *vertices =
                                static_cast<vk::VertexFormat::PositionOnly *>(task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            break;
                        }
                        case World::POSITION_COLOR: {
                            vk::VertexFormat::PositionColor *vertices =
                                static_cast<vk::VertexFormat::PositionColor *>(task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            vertex.useColorLayer = 1;
                            vertex.colorLayer = glm::vec4{
                                vertices[j].color & 0xFF,
                                (vertices[j].color >> 8) & 0xFF,
                                (vertices[j].color >> 16) & 0xFF,
                                (vertices[j].color >> 24) & 0xFF,
                            };
                            vertex.colorLayer /= 255.0;

                            break;
                        }
                        case World::LINES: {
                            vk::VertexFormat::PositionColorNormal *vertices =
                                static_cast<vk::VertexFormat::PositionColorNormal *>(task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            vertex.useColorLayer = 1;
                            vertex.colorLayer = glm::vec4{
                                vertices[j].color & 0xFF,
                                (vertices[j].color >> 8) & 0xFF,
                                (vertices[j].color >> 16) & 0xFF,
                                (vertices[j].color >> 24) & 0xFF,
                            };
                            vertex.colorLayer /= 255.0;

                            vertex.useNorm = 1;
                            vertex.norm = glm::vec3{
                                (int8_t)(vertices[j].normal & 0xFF),
                                (int8_t)((vertices[j].normal >> 8) & 0xFF),
                                (int8_t)((vertices[j].normal >> 16) & 0xFF),
                            };

                            break;
                        }
                        case World::POSITION_COLOR_LIGHT: {
                            vk::VertexFormat::PositionColorLight *vertices =
                                static_cast<vk::VertexFormat::PositionColorLight *>(task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            vertex.useColorLayer = 1;
                            vertex.colorLayer = glm::vec4{
                                vertices[j].color & 0xFF,
                                (vertices[j].color >> 8) & 0xFF,
                                (vertices[j].color >> 16) & 0xFF,
                                (vertices[j].color >> 24) & 0xFF,
                            };
                            vertex.colorLayer /= 255.0;

                            vertex.useLight = 1;
                            vertex.lightUV = glm::vec2{
                                vertices[j].uv2 & 0xFFFF,
                                (vertices[j].uv2 >> 16) & 0xFFFF,
                            };

                            break;
                        }
                        case World::POSITION_TEXTURE: {
                            vk::VertexFormat::PositionTex *vertices =
                                static_cast<vk::VertexFormat::PositionTex *>(task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            vertex.useTexture = 1;
                            vertex.textureUV = vertices[j].uv;

                            break;
                        }
                        case World::POSITION_TEXTURE_COLOR: {
                            vk::VertexFormat::PositionTexColor *vertices =
                                static_cast<vk::VertexFormat::PositionTexColor *>(task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            vertex.useTexture = 1;
                            vertex.textureUV = vertices[j].uv;

                            vertex.useColorLayer = 1;
                            vertex.colorLayer = glm::vec4{
                                vertices[j].color & 0xFF,
                                (vertices[j].color >> 8) & 0xFF,
                                (vertices[j].color >> 16) & 0xFF,
                                (vertices[j].color >> 24) & 0xFF,
                            };
                            vertex.colorLayer /= 255.0;

                            break;
                        }
                        case World::POSITION_COLOR_TEXTURE_LIGHT: {
                            vk::VertexFormat::PositionColorTexLight *vertices =
                                static_cast<vk::VertexFormat::PositionColorTexLight *>(
                                    task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            vertex.useColorLayer = 1;
                            vertex.colorLayer = glm::vec4{
                                vertices[j].color & 0xFF,
                                (vertices[j].color >> 8) & 0xFF,
                                (vertices[j].color >> 16) & 0xFF,
                                (vertices[j].color >> 24) & 0xFF,
                            };
                            vertex.colorLayer /= 255.0;

                            vertex.useTexture = 1;
                            vertex.textureUV = vertices[j].uv0;

                            vertex.useLight = 1;
                            vertex.lightUV = glm::vec2{
                                vertices[j].uv2 & 0xFFFF,
                                (vertices[j].uv2 >> 16) & 0xFFFF,
                            };

                            break;
                        }
                        case World::POSITION_TEXTURE_LIGHT_COLOR: {
                            vk::VertexFormat::PositionTexLightColor *vertices =
                                static_cast<vk::VertexFormat::PositionTexLightColor *>(
                                    task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            vertex.useTexture = 1;
                            vertex.textureUV = vertices[j].uv0;

                            vertex.useLight = 1;
                            vertex.lightUV = glm::vec2{
                                vertices[j].uv2 & 0xFFFF,
                                (vertices[j].uv2 >> 16) & 0xFFFF,
                            };

                            vertex.useColorLayer = 1;
                            vertex.colorLayer = glm::vec4{
                                vertices[j].color & 0xFF,
                                (vertices[j].color >> 8) & 0xFF,
                                (vertices[j].color >> 16) & 0xFF,
                                (vertices[j].color >> 24) & 0xFF,
                            };
                            vertex.colorLayer /= 255.0;

                            break;
                        }
                        case World::POSITION_TEXTURE_COLOR_NORMAL: {
                            vk::VertexFormat::PositionTexColorNormal *vertices =
                                static_cast<vk::VertexFormat::PositionTexColorNormal *>(
                                    task.vertices[geometryIndex + i]);

                            vertex.pos = vertices[j].position;

                            vertex.useTexture = 1;
                            vertex.textureUV = vertices[j].uv0;

                            vertex.useColorLayer = 1;
                            vertex.colorLayer = glm::vec4{
                                vertices[j].color & 0xFF,
                                (vertices[j].color >> 8) & 0xFF,
                                (vertices[j].color >> 16) & 0xFF,
                                (vertices[j].color >> 24) & 0xFF,
                            };
                            vertex.colorLayer /= 255.0;

                            vertex.useNorm = 1;
                            vertex.norm = glm::vec3{
                                (int8_t)(vertices[j].normal & 0xFF),
                                (int8_t)((vertices[j].normal >> 8) & 0xFF),
                                (int8_t)((vertices[j].normal >> 16) & 0xFF),
                            };

                            break;
                        }
                    }

                    vertex.textureID = geometryTexture;

                    geometryVertices.push_back(vertex);
                }
            }

            // Square-section roll reference: prefer the owner's local Y, then local X, then local
            // Z. Every candidate comes from the same (possibly rotated) frame, so a rotated owner
            // such as a Sable sub-level never falls back onto the world axes mid-line.
            auto orthonormalBasis = [](const glm::dvec3 &a_unit, const glm::dvec3 &refX,
                                       const glm::dvec3 &refY, const glm::dvec3 &refZ)
                -> std::pair<glm::dvec3, glm::dvec3> {
                constexpr double EPS = 1e-6;

                glm::dvec3 w = std::fabs(glm::dot(a_unit, refY)) <= 0.99 ? refY : refX;
                if (std::fabs(glm::dot(a_unit, w)) > 0.99) {
                    w = refZ;
                }

                glm::dvec3 u = glm::cross(a_unit, w);
                double uLen = glm::length(u);
                if (uLen < EPS) {
                    u = glm::cross(a_unit, refX);
                    uLen = glm::length(u);
                    if (uLen < EPS) {
                        u = glm::cross(a_unit, refZ);
                        uLen = glm::length(u);
                    }
                }
                u = uLen < EPS ? glm::dvec3(1, 0, 0) : u / uLen;
                glm::dvec3 v = glm::cross(a_unit, u);
                return {u, v};
            };

            auto cubeCornersFromFaceCenters = [orthonormalBasis](const glm::dvec3 &v1, const glm::dvec3 &v2,
                                                                 double d, const glm::dvec3 &refX,
                                                                 const glm::dvec3 &refY,
                                                                 const glm::dvec3 &refZ) -> std::pair<bool, std::array<glm::dvec3, 8>> {
                if (!(d > 0.0) || !std::isfinite(d)) { return {false, {}}; }

                glm::dvec3 axis = v2 - v1;
                double L = glm::length(axis);
                if (!(L > 0.0f)) { return {false, {}}; }

                glm::dvec3 a = glm::normalize(axis);
                auto [u, v] = orthonormalBasis(a, refX, refY, refZ);

                double h = 0.5f * d;

                // v1 面（底）
                glm::dvec3 b00 = v1 - a * 0.5 * d - u * h - v * h;
                glm::dvec3 b10 = v1 - a * 0.5 * d + u * h - v * h;
                glm::dvec3 b11 = v1 - a * 0.5 * d + u * h + v * h;
                glm::dvec3 b01 = v1 - a * 0.5 * d - u * h + v * h;

                // v2 面（顶）
                glm::dvec3 t00 = v2 + a * 0.5 * d - u * h - v * h;
                glm::dvec3 t10 = v2 + a * 0.5 * d + u * h - v * h;
                glm::dvec3 t11 = v2 + a * 0.5 * d + u * h + v * h;
                glm::dvec3 t01 = v2 + a * 0.5 * d - u * h + v * h;

                return {true, {b00, b10, b11, b01, t00, t10, t11, t01}};
            };

            const auto drawMode = static_cast<World::DrawMode>(task.indexFormats[geometryIndex + i]);
            const bool hasExplicitIndexContract = task.geometryIndexCounts != nullptr &&
                                                  task.geometryIndexTypes != nullptr &&
                                                  task.geometryIndices != nullptr;
            if (hasExplicitIndexContract) {
                geometryIndices = WorldMeshContract::triangulate(
                    static_cast<int>(drawMode), decodeDrawIndices(task, geometryIndex + i,
                                                static_cast<uint32_t>(geometryVertices.size()), drawMode));
                for (auto &vertex : geometryVertices) {
                    if (task.normalOffset && vertex.useNorm) {
                        vertex.pos += 0.00001f * glm::normalize(vertex.norm);
                    }
                    vertex.coordinate = coordinate;
                    if (post) { vertex.postBase = {x, y, z}; }
                    if (vertex.useTexture) { textureIDs.insert(vertex.textureID); }
                }
            } else switch (drawMode) {
                case World::DrawMode::TRIANGLES: {
                    for (int j = 0; j + 2 < task.vertexCounts[geometryIndex + i]; j += 3) {
                        geometryIndices.push_back(j);
                        geometryIndices.push_back(j + 1);
                        geometryIndices.push_back(j + 2);
                    }
                    for (auto &vertex : geometryVertices) {
                        if (task.normalOffset && vertex.useNorm) {
                            vertex.pos += 0.00001f * glm::normalize(vertex.norm);
                        }
                        vertex.coordinate = coordinate;
                        if (post) { vertex.postBase = {x, y, z}; }
                        if (vertex.useTexture) { textureIDs.insert(vertex.textureID); }
                    }
                    break;
                }
                case World::DrawMode::QUADS: {
                    for (int j = 0; j < task.vertexCounts[geometryIndex + i]; j += 4) {
                        geometryIndices.push_back(j + 0);
                        geometryIndices.push_back(j + 1);
                        geometryIndices.push_back(j + 2);
                        geometryIndices.push_back(j + 2);
                        geometryIndices.push_back(j + 3);
                        geometryIndices.push_back(j + 0);

                        if (task.normalOffset) {
                            if (geometryVertices[j + 0].useNorm)
                                geometryVertices[j + 0].pos +=
                                    0.00001f * glm::normalize(geometryVertices[j + 0].norm);
                            if (geometryVertices[j + 1].useNorm)
                                geometryVertices[j + 1].pos +=
                                    0.00001f * glm::normalize(geometryVertices[j + 1].norm);
                            if (geometryVertices[j + 2].useNorm)
                                geometryVertices[j + 2].pos +=
                                    0.00001f * glm::normalize(geometryVertices[j + 2].norm);
                            if (geometryVertices[j + 3].useNorm)
                                geometryVertices[j + 3].pos +=
                                    0.00001f * glm::normalize(geometryVertices[j + 3].norm);
                        }

                        geometryVertices[j + 0].coordinate = coordinate;
                        geometryVertices[j + 1].coordinate = coordinate;
                        geometryVertices[j + 2].coordinate = coordinate;
                        geometryVertices[j + 3].coordinate = coordinate;

                        if (post) {
                            geometryVertices[j + 0].postBase = {x, y, z};
                            geometryVertices[j + 1].postBase = {x, y, z};
                            geometryVertices[j + 2].postBase = {x, y, z};
                            geometryVertices[j + 3].postBase = {x, y, z};
                        }

                        if (geometryVertices[j + 3].useTexture) {
                            textureIDs.insert(geometryVertices[j + 0].textureID);
                            textureIDs.insert(geometryVertices[j + 1].textureID);
                            textureIDs.insert(geometryVertices[j + 2].textureID);
                            textureIDs.insert(geometryVertices[j + 3].textureID);
                        }
                    }

                    break;
                }
                case World::DrawMode::TRIANGLE_STRIP: {
                    std::vector<vk::VertexFormat::PBRVertex> fixedVertices;
                    for (int j = 2; j < task.vertexCounts[geometryIndex + i]; j += 2) {
                        auto v0 = geometryVertices[j - 2];
                        auto v1 = geometryVertices[j - 1];
                        auto v2 = geometryVertices[j - 1];
                        auto v3 = geometryVertices[j - 2];

                        v3.pos = geometryVertices[j].pos;
                        v2.pos = geometryVertices[j + 1].pos;
                        fixedVertices.push_back(v0);
                        fixedVertices.push_back(v1);
                        fixedVertices.push_back(v2);
                        fixedVertices.push_back(v3);
                    }
                    geometryVertices = fixedVertices;
                    for (int j = 0; j < geometryVertices.size(); j += 4) {
                        geometryIndices.push_back(j + 0);
                        geometryIndices.push_back(j + 1);
                        geometryIndices.push_back(j + 2);
                        geometryIndices.push_back(j + 2);
                        geometryIndices.push_back(j + 3);
                        geometryIndices.push_back(j + 0);

                        geometryVertices[j + 0].coordinate = coordinate;
                        geometryVertices[j + 1].coordinate = coordinate;
                        geometryVertices[j + 2].coordinate = coordinate;
                        geometryVertices[j + 3].coordinate = coordinate;

                        if (post) {
                            geometryVertices[j + 0].postBase = {x, y, z};
                            geometryVertices[j + 1].postBase = {x, y, z};
                            geometryVertices[j + 2].postBase = {x, y, z};
                            geometryVertices[j + 3].postBase = {x, y, z};
                        }
                    }
                    break;
                }
                case World::DrawMode::DEBUG_LINE_STRIP:
                case World::DrawMode::LINE_STRIP: {
                    if (!(task.lineWidth > 0.0f) || !std::isfinite(task.lineWidth)) {
                        logInvalidLineWidthOnce(geometryContentNames.back(),
                                                static_cast<World::DrawMode>(
                                                    task.indexFormats[geometryIndex + i]),
                                                task.lineWidth);
                        geometryVertices.clear();
                        break;
                    }
                    std::vector<vk::VertexFormat::PBRVertex> fixedVertices;
                    for (int j = 1; j < task.vertexCounts[geometryIndex + i]; j++) {
                        fixedVertices.push_back(geometryVertices[j - 1]);
                        fixedVertices.push_back(geometryVertices[j]);
                    }
                    geometryVertices = fixedVertices;

                    fixedVertices.clear();

                    int accu = 0;
                    for (int j = 1; j < geometryVertices.size(); j += 2) {
                        // Minecraft uses a pair of zero-alpha vertices to break one logical
                        // DEBUG_LINE_STRIP before beginning the next. OpenGL blending makes the
                        // connector invisible; an opaque ray-traced prism must not be generated
                        // for that separator segment.
                        if (geometryVertices[j - 1].colorLayer.a <= 0.0f &&
                            geometryVertices[j].colorLayer.a <= 0.0f) {
                            continue;
                        }
                        glm::dvec3 refY = lineFrameY;
                        if (outlineFrame && geometryVertices[j - 1].useNorm) {
                            const glm::vec3 &normal = geometryVertices[j - 1].norm;
                            if (glm::length(normal) > 0.5f) {
                                refY = glm::normalize(glm::dvec3(normal));
                            }
                        }
                        auto [success, cubePoints] = cubeCornersFromFaceCenters(
                            geometryVertices[j - 1].pos, geometryVertices[j].pos, task.lineWidth,
                            lineFrameX, refY, lineFrameZ);

                        if (!success) { continue; }

                        for (int k = 0; k < 8; k++) {
                            fixedVertices.push_back({
                                .pos = cubePoints[k],
                                .useColorLayer = 1,
                                .colorLayer =
                                    k < 4 ? geometryVertices[j - 1].colorLayer : geometryVertices[j].colorLayer,
                            });
                        }

                        std::vector<uint32_t> indices_ = {{0, 3, 2, 0, 2, 1,
                                                           // top (+a)
                                                           4, 5, 6, 4, 6, 7,
                                                           // -v side
                                                           0, 1, 5, 0, 5, 4,
                                                           // +u side
                                                           1, 2, 6, 1, 6, 5,
                                                           // +v side
                                                           2, 3, 7, 2, 7, 6,
                                                           // -u side
                                                           3, 0, 4, 3, 4, 7}};
                        for (int k = 0; k < 36; k++) { geometryIndices.push_back(accu + indices_[k]); }
                        accu += 8;
                    }

                    geometryVertices = fixedVertices;

                    for (int j = 0; j < geometryVertices.size(); j++) {
                        if (task.normalOffset) {
                            if (geometryVertices[j + 0].useNorm)
                                geometryVertices[j + 0].pos += 0.00001f * glm::normalize(geometryVertices[j + 0].norm);
                        }

                        geometryVertices[j + 0].coordinate = coordinate;

                        if (post) { geometryVertices[j + 0].postBase = {x, y, z}; }
                    }

                    break;
                }
                case World::DrawMode::DEBUG_LINES:
                case World::DrawMode::LINES: {
                    if (!(task.lineWidth > 0.0f) || !std::isfinite(task.lineWidth)) {
                        logInvalidLineWidthOnce(geometryContentNames.back(),
                                                static_cast<World::DrawMode>(
                                                    task.indexFormats[geometryIndex + i]),
                                                task.lineWidth);
                        geometryVertices.clear();
                        break;
                    }
                    std::vector<vk::VertexFormat::PBRVertex> fixedVertices;
                    for (int j = 0; j + 3 < task.vertexCounts[geometryIndex + i]; j += 4) {
                        fixedVertices.push_back(geometryVertices[j]);
                        fixedVertices.push_back(geometryVertices[j + 1]);
                        fixedVertices.push_back(geometryVertices[j + 2]);
                        fixedVertices.push_back(geometryVertices[j + 3]);
                        fixedVertices.push_back(geometryVertices[j + 2]);
                        fixedVertices.push_back(geometryVertices[j + 1]);
                    }
                    geometryVertices = fixedVertices;

                    fixedVertices.clear();

                    int accu = 0;
                    for (int j = 1; j < geometryVertices.size(); j += 2) {
                        glm::dvec3 refY = lineFrameY;
                        if (outlineFrame && geometryVertices[j - 1].useNorm) {
                            const glm::vec3 &normal = geometryVertices[j - 1].norm;
                            if (glm::length(normal) > 0.5f) {
                                refY = glm::normalize(glm::dvec3(normal));
                            }
                        }
                        auto [success, cubePoints] = cubeCornersFromFaceCenters(
                            geometryVertices[j - 1].pos, geometryVertices[j].pos, task.lineWidth,
                            lineFrameX, refY, lineFrameZ);

                        if (!success) { continue; }

                        for (int k = 0; k < 8; k++) {
                            fixedVertices.push_back({
                                .pos = cubePoints[k],
                                .useColorLayer = 1,
                                .colorLayer =
                                    k < 4 ? geometryVertices[j - 1].colorLayer : geometryVertices[j].colorLayer,
                            });
                        }

                        std::vector<uint32_t> indices_ = {{0, 3, 2, 0, 2, 1,
                                                           // top (+a)
                                                           4, 5, 6, 4, 6, 7,
                                                           // -v side
                                                           0, 1, 5, 0, 5, 4,
                                                           // +u side
                                                           1, 2, 6, 1, 6, 5,
                                                           // +v side
                                                           2, 3, 7, 2, 7, 6,
                                                           // -u side
                                                           3, 0, 4, 3, 4, 7}};
                        for (int k = 0; k < 36; k++) { geometryIndices.push_back(accu + indices_[k]); }
                        accu += 8;
                    }

                    geometryVertices = fixedVertices;

                    for (int j = 0; j < geometryVertices.size(); j++) {
                        if (task.normalOffset) {
                            if (geometryVertices[j + 0].useNorm)
                                geometryVertices[j + 0].pos += 0.00001f * glm::normalize(geometryVertices[j + 0].norm);
                        }

                        geometryVertices[j + 0].coordinate = coordinate;

                        if (post) { geometryVertices[j + 0].postBase = {x, y, z}; }
                    }

                    break;
                }
                default: {
                    throw std::runtime_error("Shouldn't be touched");
                }
            }

            const bool pbrSource = task.vertexFormats[geometryIndex + i] == World::PBR_TRIANGLE;
            for (auto &vertex : geometryVertices) {
                if (task.geometryAlphaModes != nullptr) {
                    vertex.alphaMode = WorldMeshContract::alphaMode(pbrSource, vertex.alphaMode,
                        static_cast<uint32_t>(task.geometryAlphaModes[geometryIndex + i]));
                }
                if (task.geometryEmissions != nullptr) {
                    vertex.albedoEmission = WorldMeshContract::emission(vertex.albedoEmission,
                        task.geometryEmissions[geometryIndex + i]);
                }
            }

            if (emissiveDebug || semanticEmission) {
                for (auto &vertex : geometryVertices) { vertex.albedoEmission = 1.0f; }
            }

            if (geometryVertices.empty() || geometryIndices.empty()) {
                vertices.pop_back();
                indices.pop_back();
                geometryMaterialFlags.pop_back();
                geometryTypes.pop_back();
                geometryGroupNames.pop_back();
                geometryContentNames.pop_back();
                shaderKeys.pop_back();
                materialKeys.pop_back();
            } else {
                allVertexCount += geometryVertices.size();
                allIndexCount += geometryIndices.size();
                geometryCountWithoutGlint++;
            }
        }

        if (!post && task.worldToken == 0) {
            composeEmissiveEyeOverlays(geometryMaterialFlags, geometryTypes, geometryGroupNames, geometryContentNames,
                                       vertices, indices, emissiveOverlayTextureIDs);
            geometryCountWithoutGlint = static_cast<uint32_t>(vertices.size());
            shaderKeys.assign(vertices.size(), "");
            materialKeys.assign(vertices.size(), "");
        } else {
            emissiveOverlayTextureIDs.assign(vertices.size(), 0u);
        }

        if (geometryCountWithoutGlint == 0) { continue; }

        std::shared_ptr<EntityBuildData> chunkBuildData =
            EntityBuildData::create(hashCode, x, y, z, rayTracingFlag, postRenderFlag, prebuiltBLAS, coordinate,
                                    geometryCountWithoutGlint,
                                    std::move(geometryTypes), std::move(geometryGroupNames),
                                    std::move(geometryContentNames), std::move(vertices), std::move(indices),
                                    std::move(emissiveOverlayTextureIDs), task.worldToken, task.frameToken,
                                    task.resourceGeneration, task.stageToken, task.stage,
                                    std::move(shaderKeys), std::move(materialKeys));

        chunkBuildData->geometryMaterialFlags = std::move(geometryMaterialFlags);
        if (post) {
            entityPostBuildDataBatch_->addData(chunkBuildData);
        } else {
            entityBuildDataBatch_->addData(chunkBuildData);
        }

        // mcvr::log::info("Entities") << "used texture ids: ";
        // for (auto id : textureIDs) { mcvr::log::info("Entities") << id << " "; }
        // mcvr::log::info("Entities") << std::endl;
    }
}

void Entities::build() {
    Renderer::instance().framework()->safeAcquireCurrentContext();
    auto framework = Renderer::instance().framework();
    auto vma = framework->vma();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();

    entityBuildDataBatch_->build();

    for (const auto &data : entityBuildDataBatch_->datas) {
        if (data != nullptr && data->auditId != 0) {
            worldMeshAuditStates_[data->auditId] = WorldMeshAuditState::Built;
        }
    }

    Renderer::instance().buffers()->queueImportantWorldUpload(entityBuildDataBatch_->indexBuffer);
    Renderer::instance().buffers()->queueImportantWorldUpload(entityBuildDataBatch_->positionBuffer);
    Renderer::instance().buffers()->queueImportantWorldUpload(entityBuildDataBatch_->materialBuffer);
    blasBatchBuilder_ = entityBuildDataBatch_->blasBatchBuilder;

    entityBatch_ = EntityBatch::create(entityBuildDataBatch_);
    if (cloudBuildData_) {
        cloudBuildData_->build();
        auto buffers = Renderer::instance().buffers();
        buffers->queueImportantWorldUpload(cloudBuildData_->indexBuffer);
        buffers->queueImportantWorldUpload(cloudBuildData_->positionBuffer);
        buffers->queueImportantWorldUpload(cloudBuildData_->materialBuffer);
        cloudBlasBuilder_ = cloudBuildData_->blasBatchBuilder;
        auto batch = EntityBatch::create(cloudBuildData_);
        cloudCache_.store(cloudRevision_, batch);
        queuedClouds_.insert(queuedClouds_.end(), batch->entities.begin(), batch->entities.end());
    }
    entityBatch_->entities.insert(entityBatch_->entities.end(), queuedClouds_.begin(), queuedClouds_.end());
    entityPostBatch_ = EntityPostBatch::create(entityPostBuildDataBatch_);

    for (auto entity : entityPostBatch_->entities) {
        for (int i = 0; i < entity->geometryCount; i++) {
            Renderer::instance().buffers()->queueImportantWorldUpload(entity->vertexBuffers[i],
                                                                      entity->indexBuffers[i]);
        }
    }
}

void Entities::close() {
    cloudCache_.clear();
    cloudBuildData_.reset();
    cloudBlasBuilder_.reset();
    queuedClouds_.clear();
    cloudCapturing_ = cloudBuildRecorded_ = false;
    for (auto &[auditId, status] : worldMeshAuditStates_) {
        if (status < WorldMeshAuditState::Built) status = WorldMeshAuditState::Stale;
    }
    worldMeshFrameOpen_ = false;
    worldMeshStages_.clear();
    activeWorldToken_ = 0;
    activeFrameToken_ = 0;
    activeResourceGeneration_ = 0;
    entityBatch_ = nullptr;
    entityPostBatch_ = nullptr;
    entityBuildDataBatch_ = nullptr;
    entityPostBuildDataBatch_ = nullptr;
    blasBatchBuilder_ = nullptr;
}

std::shared_ptr<EntityBatch> Entities::entityBatch() {
    Renderer::instance().framework()->safeAcquireCurrentContext();

    if (entityBatch_)
        return entityBatch_;
    else
        return nullptr;
}

std::shared_ptr<EntityPostBatch> Entities::entityPostBatch() {
    Renderer::instance().framework()->safeAcquireCurrentContext();

    if (entityPostBatch_)
        return entityPostBatch_;
    else
        return nullptr;
}

std::shared_ptr<vk::BLASBatchBuilder> Entities::blasBatchBuilder() {
    return blasBatchBuilder_;
}
