#include "core/render/entity_gpu_conversion.hpp"
#include "core/render/scene_scope.hpp"
#include <cstdlib>
#include <glm/gtc/type_ptr.hpp>
#include "core/diagnostics/frame_profile.hpp"
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
// CPU reference remains available for controlled comparisons (restart required).
bool gpuEntityConversionEnabled() {
    static const bool enabled = [] { const auto *v = std::getenv("MCVR_ENTITY_GPU_CONVERSION");
        return !v || std::string_view(v) != "0"; }();
    return enabled && !SceneRecordingScope::active();
}


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

void EntityBuildDataBatch::build(std::shared_ptr<vk::ComputePipeline> *conversionPipeline) {
    mcvr::profile::Scope auditProfile("entity-batch-build-record");
    auto clearBatch = [this]() {
        datas.clear();
        indexBuffer = nullptr;
        positionBuffer = nullptr;
        materialBuffer = nullptr;
        blasBatchBuilder = nullptr;
        gpuConversion = nullptr;
    };

    if (datas.empty()) {
        clearBatch();
        return;
    }

    auto framework = Renderer::instance().framework();
    auto vma = framework->vma();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();

    mcvr::profile::Phases preparation("entity.layout");
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

            totalVertexCount += data->vertexCount(i);
            totalIndexCount += data->indexCount(i);
        }

        totalGeometryCount += data->geometryCount;
    }

    if (totalGeometryCount == 0 || totalVertexCount == 0 || totalIndexCount == 0) {
        clearBatch();
        return;
    }

    preparation.next("entity.device-buffer-allocate");
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

    if (conversionPipeline) {
        gpuConversion = EntityGpuConversion::create(*framework, datas, positionBuffer, materialBuffer, indexBuffer,
                                                   *conversionPipeline);
    } else {
    // Keep the source PBR data for overlay composition and other existing consumers, but
    // write the final streams directly to their owned staging allocations. No intermediate
    // packed vectors, extra copy, global idle, or change to upload/BLAS retirement.
    preparation.next("entity.staging-write");
    positionBuffer->writeToStagingBuffer([&](void *positionData, size_t positionBytes) {
        materialBuffer->writeToStagingBuffer([&](void *materialData, size_t materialBytes) {
            indexBuffer->writeToStagingBuffer([&](void *indexData, size_t indexBytes) {
                auto positions = std::span(static_cast<vk::VertexFormat::PositionVertex *>(positionData),
                    positionBytes / sizeof(vk::VertexFormat::PositionVertex));
                auto materials = std::span(static_cast<vk::VertexFormat::MaterialVertex *>(materialData),
                    materialBytes / sizeof(vk::VertexFormat::MaterialVertex));
                auto packedIndices = std::span(static_cast<uint32_t *>(indexData), indexBytes / sizeof(uint32_t));
                size_t vertexOffset = 0, indexOffset = 0;
                preparation.next("entity.host-pack");
                for (const auto &data : datas) {
                    mcvr::profile::Scope packing("entity-pack-vertices");
                    for (size_t i = 0; i < data->vertices.size(); ++i) {
                        const auto &vertices = data->vertices[i];
                        const auto &indices = data->indices[i];
                        if (vertices.size() > positions.size() - vertexOffset ||
                            vertices.size() > materials.size() - vertexOffset ||
                            indices.size() > packedIndices.size() - indexOffset)
                            throw std::out_of_range("Entity packed stream exceeds its allocated buffer");
                        vk::Vertex::writePackedVertices(vertices, data->emissiveOverlayTextureIDs[i],
                            positions.subspan(vertexOffset, vertices.size()),
                            materials.subspan(vertexOffset, vertices.size()));
                        std::memcpy(packedIndices.data() + indexOffset, indices.data(), indices.size() * sizeof(uint32_t));
                        vertexOffset += vertices.size();
                        indexOffset += indices.size();
                    }
                }
                preparation.next("entity.staging-flush");
            });
        });
    });

    }

    preparation.next("entity.blas-prepare");
    blasBatchBuilder = vk::BLASBatchBuilder::create();
    std::vector<uint32_t> nonPrebuildInstances;
    for (int instanceIndex = 0; auto data : datas) {
        auto instanceOffset = instanceOffsets[instanceIndex];
        const mcvr::faces::ModelRules faceRules(data->geometryMaterialFlags);
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
                    positionBufferAddress, data->vertexCount(i), indexBufferAddress, data->indexCount(i),
                    data->geometryTypes[i] == World::WORLD_SOLID &&
                    !faceRules.needsAnyHit(data->geometryMaterialFlags[i]));
            }
        }
        if (data->prebuiltBLAS < 0) {
            blasGeometryBuilder->endGeometries();
            blasBuilder->defineBuildProperty(VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR)
                ->querySizeInfo(device);
        }

        instanceIndex++;
    }

    preparation.next("entity.blas-allocate");
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
        vertexCounts->push_back(chunkBuildData->vertexCount(i));
        indexCounts->push_back(chunkBuildData->indexCount(i));
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

void Entities::queueRigidModel(uint64_t model, uint64_t instance, int geometryType, int texture,
    const void *vertices, int vertexCount, double x, double y, double z, int mask,
    const float *matrix, const char *group) {
    mcvr::profile::Scope timing("entity-rigid-queue");
    if (!worldMeshFrameOpen_ || !activeWorldToken_ || !model || !(instance >> 63u)
        || !matrix || !group || !vertices || vertexCount <= 0 || vertexCount % 4)
        throw std::invalid_argument("Invalid rigid model submission");
    for (size_t i=0; i<16; ++i)
        if (!std::isfinite(matrix[i])) throw std::invalid_argument("Non-finite rigid model transform");
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        throw std::invalid_argument("Non-finite rigid model origin");
    auto framework = Renderer::instance().framework();
    auto &frr = framework->frameResourceRetainer();
    auto found = rigidModels_.find(model);
    std::shared_ptr<RigidModel> resource;
    if (found != rigidModels_.end() && found->second->world == activeWorldToken_
        && found->second->generation == activeResourceGeneration_) resource = found->second;
    if (!resource) {
        mcvr::profile::Scope creation("entity-rigid-model-create");
        if (mcvr::profile::enabled()) mcvr::profile::emit(mcvr::profile::frame, 5,
            "entity.rigid.new-input", vertexCount, static_cast<uint64_t>(vertexCount)*128);
        auto build = EntityBuildDataBatch::create();
        const auto start = entityBuildDataBatch_->datas.size();
        int hash=0, post=0, prebuilt=-1, geometryCount=1;
        int format=World::PBR_TRIANGLE, mode=static_cast<int>(World::DrawMode::QUADS);
        double origin=0;
        const char *content="";
        void *input=const_cast<void *>(vertices);
        try {
            // Use the existing material/conversion contract; a nonzero world token disables
            // the deferred dynamic-vertex path. This local mesh is expanded only on a miss.
            queueBuild(EntitiesBuildTask{.lineWidth=.0125f, .coordinate=World::WORLD,
                .normalOffset=false, .entityCount=1, .entityHashCodes=&hash,
                .entityXs=&origin, .entityYs=&origin, .entityZs=&origin,
                .entityRayTracingFlags=&mask, .entityPostRenderFlags=&post,
                .entityPrebuiltBLASs=&prebuilt, .entityPosts=&post, .entityGeometryCounts=&geometryCount,
                .geometryTypes=&geometryType, .geometryGroupNames=&group, .geometryContentNames=&content,
                .geometryTextures=&texture, .vertexFormats=&format, .indexFormats=&mode,
                .vertexCounts=&vertexCount, .vertices=&input, .worldToken=activeWorldToken_,
                .frameToken=activeFrameToken_, .resourceGeneration=activeResourceGeneration_});
            if (entityBuildDataBatch_->datas.size()!=start+1)
                throw std::logic_error("Rigid model conversion did not produce one model");
            build->datas.push_back(entityBuildDataBatch_->datas.back());
            entityBuildDataBatch_->datas.resize(start);
        } catch (...) { entityBuildDataBatch_->datas.resize(start); throw; }
        build->build();
        auto buffers=Renderer::instance().buffers();
        buffers->queueImportantWorldUpload(build->indexBuffer);
        buffers->queueImportantWorldUpload(build->positionBuffer);
        buffers->queueImportantWorldUpload(build->materialBuffer);
        resource=std::make_shared<RigidModel>();
        resource->build=build;
        resource->batch=EntityBatch::create(build);
        resource->world=activeWorldToken_;
        resource->generation=activeResourceGeneration_;
        if (found != rigidModels_.end()) frr.retain(found->second);
        // The CPU owner bounds recipes at 512. Face-state variants are additionally bounded
        // here; evicting an entry does not destroy queued instances or in-flight ownership.
        if (rigidModels_.size() >= 1024 && found == rigidModels_.end()) {
            auto oldest=std::min_element(rigidModels_.begin(),rigidModels_.end(),
                [](const auto &a,const auto &b){ return a.second->used < b.second->used; });
            frr.retain(oldest->second); rigidModels_.erase(oldest);
        }
        rigidModels_[model]=resource;
    }
    if (resource->used != activeFrameToken_) {
        rigidUsed_.push_back(resource);
        resource->used=activeFrameToken_;
    }
    auto next=std::make_shared<Entity>(*resource->batch->entities.front());
    next->rigidModel=model; next->rigidInstance=instance;
    next->x=x; next->y=y; next->z=z; next->rayTracingFlag=mask;
    next->instanceTransform=glm::make_mat4(matrix);
    next->worldToken=activeWorldToken_; next->frameToken=activeFrameToken_;
    next->resourceGeneration=activeResourceGeneration_;
    rigidInstances_.push_back(std::move(next));
}

void Entities::recordRigidModels(const std::shared_ptr<vk::CommandBuffer> &commands) {
    for (auto &model : rigidUsed_) {
        model->lifecycle.record([&] {
            model->build->blasBatchBuilder->submit(commands);
            if (mcvr::profile::enabled()) mcvr::profile::emit(mcvr::profile::frame, 5,
                "entity.rigid.blas-build", model->batch->entities.size(), 0);
        });
    }
}

void Entities::commitRigidModels() {
    auto &frr=Renderer::instance().framework()->frameResourceRetainer();
    for (auto &model : rigidUsed_) {
        if (model->lifecycle.submitted()) {
            frr.retain(model->build);
            model->build.reset();
        }
    }
}

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

void Entities::recordGpuConversion(const std::shared_ptr<vk::CommandBuffer> &commands) {
    if (mcvr::profile::enabled() && entityBuildDataBatch_) {
        size_t builds=0;
        for (const auto &data : entityBuildDataBatch_->datas) if (data->blas) ++builds;
        mcvr::profile::emit(mcvr::profile::frame,5,"entity.dynamic.blas-inputs",builds,0);
    }
    if (entityBuildDataBatch_ && entityBuildDataBatch_->gpuConversion)
        entityBuildDataBatch_->gpuConversion->record(*Renderer::instance().framework(), commands);
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
    for (auto &model : rigidUsed_) { frr.retain(model); model->lifecycle.beginFrame(); }
    rigidUsed_.clear();
    rigidInstances_.clear();
    std::erase_if(rigidModels_, [&](const auto &entry) {
        if (activeFrameToken_ <= entry.second->used + 120) return false;
        frr.retain(entry.second); return true;
    });
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
        rigidInstances_.clear();
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
    auto &retainer=Renderer::instance().framework()->frameResourceRetainer();
    for (const auto &[key, model] : rigidModels_) retainer.retain(model);
    rigidModels_.clear(); rigidInstances_.clear();
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
    mcvr::profile::Scope auditProfile("entity-convert-copy-enqueue");
    if (mcvr::profile::enabled()) {
        static constexpr const char *labels[] = {
            "entity.format.block", "entity.format.entity", "entity.format.particle", "entity.format.position",
            "entity.format.color", "entity.format.lines", "entity.format.color-light", "entity.format.tex",
            "entity.format.tex-color", "entity.format.color-tex-light", "entity.format.tex-light-color",
            "entity.format.tex-color-normal", "entity.format.pbr"};
        static_assert(std::size(labels) == World::NUM_VERTEX_FORMATS);
        std::array<uint64_t, World::NUM_VERTEX_FORMATS> counts{};
        size_t geometry = 0;
        for (int e = 0; e < task.entityCount; ++e) {
            for (int i = 0; i < task.entityGeometryCounts[e]; ++i, ++geometry) {
                const int format = task.vertexFormats[geometry];
                if (format >= 0 && format < World::NUM_VERTEX_FORMATS && task.vertexCounts[geometry] > 0)
                    counts[format] += task.vertexCounts[geometry];
            }
        }
        for (int i = 0; i < World::NUM_VERTEX_FORMATS; ++i)
            if (counts[i]) mcvr::profile::emit(mcvr::profile::frame, 5, labels[i], counts[i],
                                              counts[i] * worldVertexStride(i));
    }
    mcvr::profile::Accumulated formatPhase("entity.convert.format"),
        topologyPhase("entity.convert.topology"), materialPhase("entity.convert.material");
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
        std::vector<mcvr::EntityRawGeometry> rawGeometry;
        std::vector<uint32_t> emissiveOverlayTextureIDs;
        std::vector<std::string> shaderKeys;
        std::vector<std::string> materialKeys;
        // JNI already supplies these sizes. Reserve without changing filtering or element order.
        geometryMaterialFlags.reserve(geometryCountIncludeGlint);
        geometryTypes.reserve(geometryCountIncludeGlint);
        geometryGroupNames.reserve(geometryCountIncludeGlint);
        geometryContentNames.reserve(geometryCountIncludeGlint);
        vertices.reserve(geometryCountIncludeGlint);
        indices.reserve(geometryCountIncludeGlint);
        rawGeometry.reserve(geometryCountIncludeGlint);
        emissiveOverlayTextureIDs.reserve(geometryCountIncludeGlint);
        shaderKeys.reserve(geometryCountIncludeGlint);
        materialKeys.reserve(geometryCountIncludeGlint);
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

        bool eyeLayer = false;
        for (uint32_t i = 0; i < geometryCountIncludeGlint; ++i)
            if (task.geometryGroupNames && task.geometryGroupNames[geometryIndex+i] &&
                std::string_view(task.geometryGroupNames[geometryIndex+i]) == "eyes") eyeLayer = true;

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

            auto &raw = rawGeometry.emplace_back();
            if (gpuEntityConversionEnabled() && mcvr::rawEntityConversionEligible({
                    task.vertexFormats[geometryIndex+i], task.indexFormats[geometryIndex+i], task.vertexCounts[geometryIndex+i],
                    post, task.worldToken != 0, task.geometryIndexCounts != nullptr, eyeLayer, cloudCapturing_, prebuiltBLAS >= 0})) {
                mcvr::profile::Scope capture(formatPhase);
                const uint32_t count = task.vertexCounts[geometryIndex+i];
                raw.wordCount = mcvr::entityWordCount(static_cast<size_t>(count) * sizeof(vk::VertexFormat::PBRVertex));
                raw.words = std::make_unique_for_overwrite<uint32_t[]>(raw.wordCount);
                std::memcpy(raw.words.get(), task.vertices[geometryIndex+i], static_cast<size_t>(raw.wordCount) * 4);
                auto &job = raw.parameters;
                job.deferred = 1; job.vertexCount = count;
                job.quadIndices = task.indexFormats[geometryIndex+i] == static_cast<int>(World::DrawMode::QUADS);
                const uint64_t indexCount = job.quadIndices ? static_cast<uint64_t>(count)/4*6 : count;
                if (indexCount > UINT32_MAX) throw std::length_error("Entity index count overflow");
                job.indexCount = static_cast<uint32_t>(indexCount);
                job.normalOffset = task.normalOffset;
                job.coordinate = static_cast<uint32_t>(coordinate) & 15u;
                job.emissionPolicy = (task.geometryEmissions ? 1u : 0u) | ((emissiveDebug || semanticEmission) ? 2u : 0u);
                job.emissionBits = task.geometryEmissions ? std::bit_cast<uint32_t>(task.geometryEmissions[geometryIndex+i]) : 0u;
                // PBR alpha is already encoded. The original contract ignores the RenderType alpha override.
                allVertexCount += count; allIndexCount += job.indexCount;
                geometryCountWithoutGlint++;
                continue;
            }

            mcvr::profile::Phases conversion(formatPhase);
            if (task.vertexFormats[geometryIndex + i] == World::PBR_TRIANGLE) {
                geometryVertices.resize(task.vertexCounts[geometryIndex + i]);
                std::memcpy(geometryVertices.data(), task.vertices[geometryIndex + i],
                            task.vertexCounts[geometryIndex + i] * sizeof(vk::VertexFormat::PBRVertex));
            } else {
                geometryVertices.reserve(task.vertexCounts[geometryIndex + i]);
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

            conversion.next(topologyPhase);
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

            conversion.next(materialPhase);
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
                rawGeometry.pop_back();
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

        if (eyeLayer) rawGeometry.clear();
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
        chunkBuildData->rawGeometry = std::move(rawGeometry);
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
    mcvr::profile::Scope auditProfile("entity-build-dispatch");
    Renderer::instance().framework()->safeAcquireCurrentContext();
    auto framework = Renderer::instance().framework();
    auto vma = framework->vma();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();

    entityBuildDataBatch_->build(gpuEntityConversionEnabled() ? &conversionPipeline_ : nullptr);

    for (const auto &data : entityBuildDataBatch_->datas) {
        if (data != nullptr && data->auditId != 0) {
            worldMeshAuditStates_[data->auditId] = WorldMeshAuditState::Built;
        }
    }

    if (auto conversion = entityBuildDataBatch_->gpuConversion) {
        Renderer::instance().buffers()->queueImportantWorldUpload(conversion->input);
        Renderer::instance().buffers()->queueImportantWorldUpload(conversion->jobs);
    } else {
    Renderer::instance().buffers()->queueImportantWorldUpload(entityBuildDataBatch_->indexBuffer);
    Renderer::instance().buffers()->queueImportantWorldUpload(entityBuildDataBatch_->positionBuffer);
    Renderer::instance().buffers()->queueImportantWorldUpload(entityBuildDataBatch_->materialBuffer);
    }
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
    entityBatch_->entities.insert(entityBatch_->entities.end(), rigidInstances_.begin(), rigidInstances_.end());
    if (mcvr::profile::enabled()) {
        uint64_t vertices=0;
        for (const auto &entity : rigidInstances_)
            for (auto count : *entity->vertexCounts) vertices+=count;
        mcvr::profile::emit(mcvr::profile::frame,5,"entity.rigid.instances",rigidInstances_.size(),0);
        mcvr::profile::emit(mcvr::profile::frame,5,"entity.rigid.referenced-vertices",vertices,0);
        mcvr::profile::emit(mcvr::profile::frame,5,"entity.rigid.resident-models",rigidModels_.size(),0);
    }
    entityPostBatch_ = EntityPostBatch::create(entityPostBuildDataBatch_);

    for (auto entity : entityPostBatch_->entities) {
        for (int i = 0; i < entity->geometryCount; i++) {
            Renderer::instance().buffers()->queueImportantWorldUpload(entity->vertexBuffers[i],
                                                                      entity->indexBuffers[i]);
        }
    }
}

void Entities::close() {
    rigidModels_.clear(); rigidInstances_.clear(); rigidUsed_.clear();
    conversionPipeline_.reset();
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
