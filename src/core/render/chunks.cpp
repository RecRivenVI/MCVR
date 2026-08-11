#include "core/render/chunk_trace.hpp"
#include "core/render/material_faces.hpp"
#include "core/render/chunk_scheduling.hpp"
#include "core/render/chunks.hpp"
#include "core/failure_state.hpp"

#include "core/diagnostics/device_loss_trace.hpp"
#include "core/render/buffers.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/vulkan/vertex.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
struct ChunkPerformanceStats {
    std::atomic<uint64_t> buildInputCount{0};
    std::atomic<uint64_t> buildInputCpuNs{0};
    std::atomic<uint64_t> batchBuildCount{0};
    std::atomic<uint64_t> batchBuildCpuNs{0};
    std::atomic<uint64_t> batchSubmitCount{0};
    std::atomic<uint64_t> batchSubmitCpuNs{0};
    std::atomic<uint64_t> batchCompleteCount{0};
    std::atomic<uint64_t> batchQueueGpuWallNs{0};
};

ChunkPerformanceStats chunkPerformance;
const bool chunkPerformanceEnabled = std::getenv("RADIANCE_CHUNK_PERF") != nullptr;

uint64_t elapsedNs(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count());
}
}

struct LightData {
    glm::vec4 p0Area;
    glm::vec4 p1;
    glm::vec4 p2;
    glm::vec4 p3;
    glm::vec4 normal;
    glm::vec4 radiance;
    glm::vec4 sourceIDData;
};
static_assert(sizeof(LightData) == sizeof(glm::vec4) * 7);

static void buildChunkPackedVertices(const std::vector<std::vector<vk::VertexFormat::PBRVertex>> &vertices,
                                     const std::vector<std::vector<uint32_t>> &indices,
                                     std::vector<vk::VertexFormat::PositionVertex> &packedPositions,
                                     std::vector<vk::VertexFormat::MaterialVertex> &packedMaterials,
                                     std::vector<uint32_t> &packedIndices) {
    for (int i = 0; i < static_cast<int>(vertices.size()); i++) {
        const auto &geometryVertices = vertices[i];
        const auto &geometryIndices = indices[i];

        packedIndices.insert(packedIndices.end(), geometryIndices.begin(), geometryIndices.end());

        for (const auto &vertex : geometryVertices) {
            packedPositions.push_back(vk::Vertex::makePositionVertex(vertex));
            packedMaterials.push_back(vk::Vertex::makeMaterialVertex(vertex));
        }
    }
}

static uint64_t hashCombine64(uint64_t seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    return seed;
}

static float packUintBits(uint32_t value) {
    float packed = 0.0f;
    std::memcpy(&packed, &value, sizeof(uint32_t));
    return packed;
}

static float triangleArea(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c) {
    return 0.5f * glm::length(glm::cross(b - a, c - a));
}

static float cross2D(const glm::vec2 &a, const glm::vec2 &b) {
    return a.x * b.y - a.y * b.x;
}

static void clipUvPolygonAgainstEdge(const std::vector<glm::vec2> &input,
                                     std::vector<glm::vec2> &output,
                                     int axis,
                                     float value,
                                     bool keepGreater) {
    output.clear();
    if (input.empty()) {
        return;
    }

    auto coordinate = [&](const glm::vec2 &uv) -> float { return axis == 0 ? uv.x : uv.y; };
    auto inside = [&](const glm::vec2 &uv) -> bool {
        return keepGreater ? coordinate(uv) >= value : coordinate(uv) <= value;
    };
    auto intersect = [&](const glm::vec2 &a, const glm::vec2 &b) -> glm::vec2 {
        float delta = coordinate(b) - coordinate(a);
        if (std::abs(delta) <= 1e-8f) {
            return a;
        }
        float t = (value - coordinate(a)) / delta;
        return glm::mix(a, b, std::clamp(t, 0.0f, 1.0f));
    };

    glm::vec2 previous = input.back();
    bool previousInside = inside(previous);
    for (const glm::vec2 &current : input) {
        bool currentInside = inside(current);
        if (currentInside != previousInside) {
            output.push_back(intersect(previous, current));
        }
        if (currentInside) {
            output.push_back(current);
        }
        previous = current;
        previousInside = currentInside;
    }
}

static std::vector<glm::vec2> clipUvTriangleToRect(const std::array<glm::vec2, 3> &triangle,
                                                   const glm::vec2 &rectMin,
                                                   const glm::vec2 &rectMax) {
    std::vector<glm::vec2> polygon(triangle.begin(), triangle.end());
    std::vector<glm::vec2> scratch;

    clipUvPolygonAgainstEdge(polygon, scratch, 0, rectMin.x, true);
    polygon.swap(scratch);
    clipUvPolygonAgainstEdge(polygon, scratch, 0, rectMax.x, false);
    polygon.swap(scratch);
    clipUvPolygonAgainstEdge(polygon, scratch, 1, rectMin.y, true);
    polygon.swap(scratch);
    clipUvPolygonAgainstEdge(polygon, scratch, 1, rectMax.y, false);
    polygon.swap(scratch);
    return polygon;
}

static void releaseChunkBuildDataStaging(const std::shared_ptr<ChunkBuildData> &chunkBuildData) {
    if (chunkBuildData->indexBuffer != nullptr) {
        chunkBuildData->indexBuffer->releaseStaging();
    }
    if (chunkBuildData->positionBuffer != nullptr) {
        chunkBuildData->positionBuffer->releaseStaging();
    }
    if (chunkBuildData->materialBuffer != nullptr) {
        chunkBuildData->materialBuffer->releaseStaging();
    }
    if (chunkBuildData->lightBuffer != nullptr) {
        chunkBuildData->lightBuffer->releaseStaging();
    }
}

static bool barycentricFromUv(const glm::vec2 &uv,
                              const std::array<glm::vec2, 3> &triangleUv,
                              glm::vec3 &outBarycentric) {
    glm::vec2 e0 = triangleUv[1] - triangleUv[0];
    glm::vec2 e1 = triangleUv[2] - triangleUv[0];
    float denominator = cross2D(e0, e1);
    if (std::abs(denominator) <= 1e-8f) {
        return false;
    }

    glm::vec2 relative = uv - triangleUv[0];
    float b1 = cross2D(relative, e1) / denominator;
    float b2 = cross2D(e0, relative) / denominator;
    outBarycentric = glm::vec3(1.0f - b1 - b2, b1, b2);
    return true;
}

static bool uvToTrianglePosition(const glm::vec2 &uv,
                                 const std::array<glm::vec2, 3> &triangleUv,
                                 const std::array<glm::vec3, 3> &trianglePos,
                                 glm::vec3 &outPosition) {
    glm::vec3 barycentric;
    if (!barycentricFromUv(uv, triangleUv, barycentric)) {
        return false;
    }

    outPosition = trianglePos[0] * barycentric.x + trianglePos[1] * barycentric.y + trianglePos[2] * barycentric.z;
    return true;
}

static bool uvToTriangleTint(const glm::vec2 &uv,
                             const std::array<glm::vec2, 3> &triangleUv,
                             const std::array<glm::vec3, 3> &triangleTint,
                             glm::vec3 &outTint) {
    glm::vec3 barycentric;
    if (!barycentricFromUv(uv, triangleUv, barycentric)) {
        return false;
    }

    outTint = triangleTint[0] * barycentric.x + triangleTint[1] * barycentric.y + triangleTint[2] * barycentric.z;
    return true;
}

static glm::vec3 emissionVertexTint(const vk::VertexFormat::PBRVertex &vertex) {
    if (vertex.useColorLayer == 0) {
        return glm::vec3(1.0f);
    }

    return glm::clamp(glm::vec3(vertex.colorLayer), glm::vec3(0.0f), glm::vec3(1.0f));
}

static uint64_t buildChunkLightStableID(int64_t chunkId,
                                        uint32_t geometryIndex,
                                        uint32_t quadIndex,
                                        uint64_t cellStableKey) {
    uint64_t seed = 0xcbf29ce484222325ull;
    seed = hashCombine64(seed, static_cast<uint64_t>(chunkId));
    seed = hashCombine64(seed, static_cast<uint64_t>(geometryIndex));
    seed = hashCombine64(seed, static_cast<uint64_t>(quadIndex));
    seed = hashCombine64(seed, cellStableKey);
    return seed;
}

static ChunkPackedData makeChunkPackedData(int32_t x,
                                           int32_t y,
                                           int32_t z,
                                           uint32_t geometryCount,
                                           uint32_t lightCount,
                                           VkDeviceAddress lightBufferAddress) {
    return {
        .x = x,
        .y = y,
        .z = z,
        .geometryCount = geometryCount,
        .lightCount = lightCount,
        .lightBufferAddress = lightBufferAddress,
    };
}

static void storeChunkPackedData(std::vector<ChunkPackedData> &chunkPackedData,
                                 int64_t id,
                                 int32_t x,
                                 int32_t y,
                                 int32_t z,
                                 uint32_t geometryCount,
                                 uint32_t lightCount,
                                 VkDeviceAddress lightBufferAddress) {
    if (!Renderer::options.collectChunkEmission) {
        return;
    }

    chunkPackedData[id] = makeChunkPackedData(x, y, z, geometryCount, lightCount, lightBufferAddress);
}

static LightData packLight(const LightInfo &light, const glm::vec3 &chunkOrigin) {
    LightData gpuLight{};
    glm::vec3 p0 = light.p0 + chunkOrigin;
    glm::vec3 p1 = light.p1 + chunkOrigin;
    glm::vec3 p2 = light.p2 + chunkOrigin;
    glm::vec3 p3 = light.p3 + chunkOrigin;

    gpuLight.p0Area = glm::vec4(p0, light.area);
    gpuLight.p1 = glm::vec4(p1, 0.0f);
    gpuLight.p2 = glm::vec4(p2, 0.0f);
    gpuLight.p3 = glm::vec4(p3, 0.0f);
    gpuLight.normal = glm::vec4(light.normal, 0.0f);
    gpuLight.radiance = glm::vec4(light.radiance, 0.0f);
    uint32_t stableIDLow = static_cast<uint32_t>(light.stableID & 0xffffffffull);
    uint32_t stableIDHigh = static_cast<uint32_t>((light.stableID >> 32u) & 0xffffffffull);
    gpuLight.sourceIDData = glm::vec4(packUintBits(stableIDLow), packUintBits(stableIDHigh), 0.0f, 0.0f);
    return gpuLight;
}

void ChunkBuildData::buildLightBuffer(const std::shared_ptr<vk::VMA> &vma,
                                      const std::shared_ptr<vk::Device> &device,
                                      bool persistStaging) {
    lightBuffer = nullptr;
    lightCount = 0;

    if (!collectChunkEmission) {
        (void)vma;
        (void)device;
        return;
    }

    if (lightInfos.empty()) {
        return;
    }

    std::vector<LightData> gpuLights;
    gpuLights.reserve(lightInfos.size());

    glm::vec3 chunkOrigin(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    for (const auto &light : lightInfos) {
        float maxRadiance = std::max({light.radiance.r, light.radiance.g, light.radiance.b});
        if (light.area <= 1e-6f || maxRadiance <= 1e-6f) {
            continue;
        }

        gpuLights.push_back(packLight(light, chunkOrigin));
    }

    if (gpuLights.empty()) {
        return;
    }

    lightCount = static_cast<uint32_t>(gpuLights.size());

    size_t lightBytes = static_cast<size_t>(lightCount) * sizeof(LightData);
    lightBuffer = vk::DeviceLocalBuffer::create(vma, device, persistStaging, lightBytes,
                                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                0, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                                16);
#ifdef DEBUG
    if ((lightBuffer->bufferAddress() & (16 - 1)) != 0) {
        throw std::runtime_error("Chunk light buffer address is not 16-byte aligned");
    }
#endif
    lightBuffer->uploadToStagingBuffer(gpuLights.data(), lightBytes, 0);
}

ChunkBuildData::ChunkBuildData(int64_t id,
                               int x,
                               int y,
                               int z,
                               int64_t version,
                               bool collectChunkEmission,
                               uint32_t allVertexCount,
                               uint32_t allIndexCount,
                               uint32_t geometryCount,
                               std::vector<World::GeometryTypes> &&geometryTypes,
                               std::vector<std::string> &&geometryGroupNames,
                               std::vector<uint32_t> &&geometryMaterialFlags,
                               std::vector<std::vector<vk::VertexFormat::PBRVertex>> &&vertices,
                               std::vector<std::vector<uint32_t>> &&indices)
    : id(id),
      x(x),
      y(y),
      z(z),
      version(version),
      collectChunkEmission(collectChunkEmission),
      allVertexCount(allVertexCount),
      allIndexCount(allIndexCount),
      geometryCount(geometryCount),
      geometryTypes(std::move(geometryTypes)),
      geometryGroupNames(std::move(geometryGroupNames)),
      geometryMaterialFlags(std::move(geometryMaterialFlags)),
      vertices(std::move(vertices)),
      indices(std::move(indices)),
      indexBufferAddresses(),
      positionBufferAddresses(),
      materialBufferAddresses(),
      indexBuffer(nullptr),
      positionBuffer(nullptr),
      materialBuffer(nullptr),
      blas(nullptr),
      blasBuilder(nullptr),
      lightInfos(),
      lightBuffer(nullptr),
      lightCount(0) {}

void ChunkBuildData::buildLightInfos(const Emission &emission) {
    lightInfos.clear();

    if (!collectChunkEmission) {
        (void)emission;
        return;
    }

    for (uint32_t geometryIndex = 0; geometryIndex < vertices.size(); geometryIndex++) {
        auto &geometryVertices = vertices[geometryIndex];
        if (geometryVertices.size() < 4) {
            continue;
        }

        uint32_t quadIndex = 0;
        for (size_t vertexIndex = 0; vertexIndex + 3 < geometryVertices.size(); vertexIndex += 4, quadIndex++) {
            const auto &v0 = geometryVertices[vertexIndex + 0];
            const auto &v1 = geometryVertices[vertexIndex + 1];
            const auto &v2 = geometryVertices[vertexIndex + 2];
            const auto &v3 = geometryVertices[vertexIndex + 3];

            glm::vec3 tint0 = emissionVertexTint(v0);
            glm::vec3 tint1 = emissionVertexTint(v1);
            glm::vec3 tint2 = emissionVertexTint(v2);
            glm::vec3 tint3 = emissionVertexTint(v3);

            if (v0.useTexture == 0 || v1.useTexture == 0 || v2.useTexture == 0 || v3.useTexture == 0) {
                continue;
            }

            std::array<glm::vec2, 4> quadUv = {v0.textureUV, v1.textureUV, v2.textureUV, v3.textureUV};
            glm::vec2 uvMin(std::numeric_limits<float>::max());
            glm::vec2 uvMax(std::numeric_limits<float>::lowest());
            for (glm::vec2 uv : quadUv) {
                uvMin = glm::min(uvMin, uv);
                uvMax = glm::max(uvMax, uv);
            }

            glm::vec2 uvSize = uvMax - uvMin;
            if (uvSize.x <= 1e-6f || uvSize.y <= 1e-6f) {
                continue;
            }

            glm::vec3 triNormal0 = glm::cross(v1.pos - v0.pos, v2.pos - v0.pos);
            glm::vec3 triNormal1 = glm::cross(v2.pos - v0.pos, v3.pos - v0.pos);
            glm::vec3 baseNormal = triNormal0 + triNormal1;
            float baseNormalLength = glm::length(baseNormal);
            if (baseNormalLength <= 1e-6f) {
                continue;
            }

            glm::vec3 normal = baseNormal / baseNormalLength;
            float quadArea = triangleArea(v0.pos, v1.pos, v2.pos) + triangleArea(v0.pos, v2.pos, v3.pos);
            if (quadArea <= 1e-6f) {
                continue;
            }

            thread_local std::vector<std::shared_ptr<const EmissionCell>> candidateCells;
            emission.collectCells(v0.textureID, uvMin, uvMax, candidateCells);
            for (const auto &candidateCell : candidateCells) {
                if (candidateCell == nullptr) {
                    continue;
                }

                const auto &cell = *candidateCell;
                glm::vec2 overlapMin = glm::max(uvMin, cell.uvMin);
                glm::vec2 overlapMax = glm::min(uvMax, cell.uvMax);
                glm::vec2 overlapSize = overlapMax - overlapMin;
                if (overlapSize.x <= 1e-6f || overlapSize.y <= 1e-6f || cell.avgEmission <= 0.0f) {
                    continue;
                }

                uint64_t baseStableID = buildChunkLightStableID(id, geometryIndex, quadIndex, cell.stableKey);
                std::array<std::array<glm::vec2, 3>, 2> triangleUv = {{
                    {v0.textureUV, v1.textureUV, v2.textureUV},
                    {v0.textureUV, v2.textureUV, v3.textureUV},
                }};
                std::array<std::array<glm::vec3, 3>, 2> trianglePos = {{
                    {v0.pos, v1.pos, v2.pos},
                    {v0.pos, v2.pos, v3.pos},
                }};
                std::array<std::array<glm::vec3, 3>, 2> triangleTint = {{
                    {tint0, tint1, tint2},
                    {tint0, tint2, tint3},
                }};

                uint64_t trianglePieceIndex = 0;
                for (int triangleIndex = 0; triangleIndex < 2; ++triangleIndex) {
                    std::vector<glm::vec2> clippedPolygon =
                        clipUvTriangleToRect(triangleUv[triangleIndex], overlapMin, overlapMax);
                    if (clippedPolygon.size() < 3) {
                        continue;
                    }

                    glm::vec3 fanOrigin;
                    glm::vec3 fanOriginTint;
                    if (!uvToTrianglePosition(clippedPolygon[0], triangleUv[triangleIndex], trianglePos[triangleIndex],
                                              fanOrigin) ||
                        !uvToTriangleTint(clippedPolygon[0], triangleUv[triangleIndex], triangleTint[triangleIndex],
                                          fanOriginTint)) {
                        continue;
                    }

                    for (size_t polygonIndex = 1; polygonIndex + 1 < clippedPolygon.size(); ++polygonIndex) {
                        glm::vec3 p1;
                        glm::vec3 p2;
                        glm::vec3 p1Tint;
                        glm::vec3 p2Tint;
                        if (!uvToTrianglePosition(clippedPolygon[polygonIndex], triangleUv[triangleIndex],
                                                  trianglePos[triangleIndex], p1) ||
                            !uvToTriangleTint(clippedPolygon[polygonIndex], triangleUv[triangleIndex],
                                              triangleTint[triangleIndex], p1Tint) ||
                            !uvToTrianglePosition(clippedPolygon[polygonIndex + 1], triangleUv[triangleIndex],
                                                  trianglePos[triangleIndex], p2) ||
                            !uvToTriangleTint(clippedPolygon[polygonIndex + 1], triangleUv[triangleIndex],
                                              triangleTint[triangleIndex], p2Tint)) {
                            continue;
                        }

                        float triangleLightArea = triangleArea(fanOrigin, p1, p2);
                        if (triangleLightArea <= 1e-6f) {
                            continue;
                        }

                        glm::vec3 lightNormal = glm::cross(p1 - fanOrigin, p2 - fanOrigin);
                        float lightNormalLength = glm::length(lightNormal);
                        lightNormal = lightNormalLength > 1e-6f ? lightNormal / lightNormalLength : normal;
                        glm::vec3 avgTint = glm::max((fanOriginTint + p1Tint + p2Tint) / 3.0f, glm::vec3(0.0f));

                        LightInfo info{
                            .p0 = fanOrigin,
                            .p1 = p1,
                            .p2 = p2,
                            .p3 = p2,
                            .normal = lightNormal,
                            .radiance = cell.avgColor * avgTint * cell.avgEmission,
                            .area = triangleLightArea,
                            .textureID = v0.textureID,
                            .stableID = hashCombine64(baseStableID, trianglePieceIndex++),
                        };
                        lightInfos.push_back(info);
                    }
                }
            }
        }
    }
}

void ChunkBuildData::build(bool persistStaging) {
    auto framework = Renderer::instance().framework();
    auto vma = framework->vma();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();

    buildLightBuffer(vma, device, persistStaging);

    if (geometryCount == 0) {
        blas = nullptr;
        blasBuilder = nullptr;
        return;
    }

    std::vector<uint32_t> geometryVertexOffsets;
    std::vector<uint32_t> geometryIndexOffsets;
    geometryVertexOffsets.reserve(geometryCount);
    geometryIndexOffsets.reserve(geometryCount);

    uint32_t totalVertexCount = 0;
    uint32_t totalIndexCount = 0;
    for (int i = 0; i < geometryCount; i++) {
        geometryVertexOffsets.push_back(totalVertexCount);
        geometryIndexOffsets.push_back(totalIndexCount);
        totalVertexCount += vertices[i].size();
        totalIndexCount += indices[i].size();
    }

    positionBuffer = vk::DeviceLocalBuffer::create(vma, device, persistStaging,
                                                   totalVertexCount * sizeof(vk::VertexFormat::PositionVertex),
                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    materialBuffer = vk::DeviceLocalBuffer::create(vma, device, persistStaging,
                                                   totalVertexCount * sizeof(vk::VertexFormat::MaterialVertex),
                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    indexBuffer = vk::DeviceLocalBuffer::create(
        vma, device, persistStaging, totalIndexCount * sizeof(uint32_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    std::vector<vk::VertexFormat::PositionVertex> packedPositions;
    std::vector<vk::VertexFormat::MaterialVertex> packedMaterials;
    std::vector<uint32_t> packedIndices;
    packedPositions.reserve(totalVertexCount);
    packedMaterials.reserve(totalVertexCount);
    packedIndices.reserve(totalIndexCount);
    buildChunkPackedVertices(vertices, indices, packedPositions, packedMaterials, packedIndices);

    if (!packedPositions.empty()) {
        positionBuffer->uploadToStagingBuffer(packedPositions.data(),
                                              packedPositions.size() * sizeof(vk::VertexFormat::PositionVertex), 0);
        materialBuffer->uploadToStagingBuffer(packedMaterials.data(),
                                              packedMaterials.size() * sizeof(vk::VertexFormat::MaterialVertex), 0);
    }
    if (!packedIndices.empty()) {
        indexBuffer->uploadToStagingBuffer(packedIndices.data(), packedIndices.size() * sizeof(uint32_t), 0);
    }

    indexBufferAddresses.reserve(geometryCount);
    positionBufferAddresses.reserve(geometryCount);
    materialBufferAddresses.reserve(geometryCount);

    blasBuilder = vk::BLASBuilder::create();
    auto blasGeometryBuilder = blasBuilder->beginGeometries();
    for (int i = 0; i < geometryCount; i++) {
        const VkDeviceAddress geometryIndexAddress =
            indexBuffer->bufferAddress() + geometryIndexOffsets[i] * sizeof(uint32_t);
        const VkDeviceAddress geometryPositionAddress =
            positionBuffer->bufferAddress() + geometryVertexOffsets[i] * sizeof(vk::VertexFormat::PositionVertex);
        const VkDeviceAddress geometryMaterialAddress =
            materialBuffer->bufferAddress() + geometryVertexOffsets[i] * sizeof(vk::VertexFormat::MaterialVertex);

        indexBufferAddresses.push_back(geometryIndexAddress);
        positionBufferAddresses.push_back(geometryPositionAddress);
        materialBufferAddresses.push_back(geometryMaterialAddress);

        blasGeometryBuilder->defineTriangleGeomrtry<vk::VertexFormat::PositionVertex>(
            geometryPositionAddress, vertices[i].size(), geometryIndexAddress, indices[i].size(),
            geometryTypes[i] == World::WORLD_SOLID && !mcvr::faces::needsAnyHit(geometryMaterialFlags[i], geometryMaterialFlags));
    }

    positionBuffer->flushStagingBuffer();
    materialBuffer->flushStagingBuffer();
    indexBuffer->flushStagingBuffer();

    blasGeometryBuilder->endGeometries();
    blas = blasBuilder->defineBuildProperty(VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR)
               ->querySizeInfo(device)
               ->allocateBuffers(physicalDevice, device, vma)
               ->build(device);
}

ChunkBuildDataBatch::ChunkBuildDataBatch(std::vector<std::shared_ptr<ChunkBuildData>> &&batchData)
    : batchData(std::move(batchData)) {}

ChunkBuildDataBatch::ChunkBuildDataBatch(uint32_t maxBatchSize,
                                         std::set<int64_t> &queuedIndexSet,
                                         std::vector<std::shared_ptr<Chunk1>> &chunks,
                                         std::vector<std::shared_ptr<ChunkBuildData>> &chunkBuildDatas,
                                         glm::vec3 cameraPos, uint64_t inFlightBytes, uint64_t admissionSequence) {
    auto currentTime = std::chrono::steady_clock::now();
    auto queuedChunkPos = [&](int64_t id) -> glm::vec3 {
        const auto &chunkBuildData = chunkBuildDatas[id];
        if (chunkBuildData != nullptr) {
            return {
                static_cast<float>(chunkBuildData->x),
                static_cast<float>(chunkBuildData->y),
                static_cast<float>(chunkBuildData->z),
            };
        }

        return {
            static_cast<float>(chunks[id]->x),
            static_cast<float>(chunks[id]->y),
            static_cast<float>(chunks[id]->z),
        };
    };
    auto key = [&](int64_t id) {
        const auto &data = chunkBuildDatas[id];
        return mcvr::chunkScheduling::Key{data->priority, data->queuedAt,
            glm::distance(cameraPos, queuedChunkPos(id))};
    };
    auto bytesOf = [&](int64_t id) {
        const auto &data = chunkBuildDatas[id];
        return uint64_t(data->allVertexCount)*sizeof(vk::VertexFormat::PBRVertex)
            + uint64_t(data->allIndexCount)*sizeof(uint32_t)
            + uint64_t(data->lightCount)*sizeof(LightInfo);
    };
    auto selected = mcvr::chunkScheduling::selectBatch(queuedIndexSet, maxBatchSize,
        currentTime, key, bytesOf, inFlightBytes, admissionSequence);
    for (auto id : selected) {
        queuedIndexSet.erase(id);
        batchData.push_back(std::exchange(chunkBuildDatas[id], nullptr));
    }
}

void ChunkBuildDataBatch::build() {
    auto framework = Renderer::instance().framework();
    auto vma = framework->vma();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();

    for (auto &data : batchData) {
        if (data == nullptr) {
            continue;
        }
        data->buildLightBuffer(vma, device, true);
    }

    std::vector<uint32_t> instanceOffsets;
    std::vector<uint32_t> geometryVertexOffsets;
    std::vector<uint32_t> geometryIndexOffsets;
    instanceOffsets.reserve(batchData.size());

    uint32_t totalGeometryCount = 0;
    uint32_t totalVertexCount = 0;
    uint32_t totalIndexCount = 0;

    for (const auto &data : batchData) {
        if (data == nullptr) {
            instanceOffsets.push_back(totalGeometryCount);
            continue;
        }
        instanceOffsets.push_back(totalGeometryCount);
        for (int i = 0; i < data->geometryCount; i++) {
            geometryVertexOffsets.push_back(totalVertexCount);
            geometryIndexOffsets.push_back(totalIndexCount);
            totalVertexCount += data->vertices[i].size();
            totalIndexCount += data->indices[i].size();
        }
        totalGeometryCount += data->geometryCount;
    }

    if (totalGeometryCount == 0) {
        for (const auto &data : batchData) {
            data->indexBufferAddresses.clear();
            data->positionBufferAddresses.clear();
            data->materialBufferAddresses.clear();
            data->indexBuffer = nullptr;
            data->positionBuffer = nullptr;
            data->materialBuffer = nullptr;
            data->blas = nullptr;
            data->blasBuilder = nullptr;
        }
        return;
    }

    positionBuffer = vk::DeviceLocalBuffer::create(vma, device, true,
                                                   totalVertexCount * sizeof(vk::VertexFormat::PositionVertex),
                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    materialBuffer = vk::DeviceLocalBuffer::create(vma, device, true,
                                                   totalVertexCount * sizeof(vk::VertexFormat::MaterialVertex),
                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    indexBuffer = vk::DeviceLocalBuffer::create(
        vma, device, true, totalIndexCount * sizeof(uint32_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    std::vector<vk::VertexFormat::PositionVertex> packedPositions;
    std::vector<vk::VertexFormat::MaterialVertex> packedMaterials;
    std::vector<uint32_t> packedIndices;
    packedPositions.reserve(totalVertexCount);
    packedMaterials.reserve(totalVertexCount);
    packedIndices.reserve(totalIndexCount);
    for (const auto &data : batchData) {
        if (data == nullptr) {
            continue;
        }
        buildChunkPackedVertices(data->vertices, data->indices, packedPositions, packedMaterials, packedIndices);
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

    blasBatchBuilder = vk::BLASBatchBuilder::create();
    std::vector<uint32_t> nonEmptyInstanceIndices;
    nonEmptyInstanceIndices.reserve(batchData.size());

    for (int chunkIndex = 0; auto &data : batchData) {
        if (data == nullptr) {
            chunkIndex++;
            continue;
        }
        const auto instanceOffset = instanceOffsets[chunkIndex];
        data->indexBufferAddresses.clear();
        data->positionBufferAddresses.clear();
        data->materialBufferAddresses.clear();
        data->indexBufferAddresses.reserve(data->geometryCount);
        data->positionBufferAddresses.reserve(data->geometryCount);
        data->materialBufferAddresses.reserve(data->geometryCount);

        if (data->geometryCount == 0) {
            data->indexBuffer = nullptr;
            data->positionBuffer = nullptr;
            data->materialBuffer = nullptr;
            data->blas = nullptr;
            data->blasBuilder = nullptr;
            chunkIndex++;
            continue;
        }

        data->indexBuffer = indexBuffer;
        data->positionBuffer = positionBuffer;
        data->materialBuffer = materialBuffer;

        auto blasBuilder = blasBatchBuilder->defineBLASBuilder();
        auto blasGeometryBuilder = blasBuilder->beginGeometries();

        for (int i = 0; i < data->geometryCount; i++) {
            const VkDeviceAddress geometryIndexAddress =
                indexBuffer->bufferAddress() + geometryIndexOffsets[instanceOffset + i] * sizeof(uint32_t);
            const VkDeviceAddress geometryPositionAddress =
                positionBuffer->bufferAddress() +
                geometryVertexOffsets[instanceOffset + i] * sizeof(vk::VertexFormat::PositionVertex);
            const VkDeviceAddress geometryMaterialAddress =
                materialBuffer->bufferAddress() +
                geometryVertexOffsets[instanceOffset + i] * sizeof(vk::VertexFormat::MaterialVertex);

            data->indexBufferAddresses.push_back(geometryIndexAddress);
            data->positionBufferAddresses.push_back(geometryPositionAddress);
            data->materialBufferAddresses.push_back(geometryMaterialAddress);

            blasGeometryBuilder->defineTriangleGeomrtry<vk::VertexFormat::PositionVertex>(
                geometryPositionAddress, data->vertices[i].size(), geometryIndexAddress, data->indices[i].size(),
                data->geometryTypes[i] == World::WORLD_SOLID && !mcvr::faces::needsAnyHit(data->geometryMaterialFlags[i], data->geometryMaterialFlags));
        }

        blasGeometryBuilder->endGeometries();
        data->blasBuilder = blasBuilder->defineBuildProperty(VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR)
                                ->querySizeInfo(device);
        nonEmptyInstanceIndices.push_back(chunkIndex);
        chunkIndex++;
    }

    positionBuffer->flushStagingBuffer();
    materialBuffer->flushStagingBuffer();
    indexBuffer->flushStagingBuffer();

    auto blases = blasBatchBuilder->allocateBuffers(physicalDevice, device, vma)->build(device);
    for (int i = 0; i < nonEmptyInstanceIndices.size(); i++) {
        batchData[nonEmptyInstanceIndices[i]]->blas = blases[i];
    }
}

ChunkBuildScheduler::ChunkBuildScheduler(std::set<int64_t> &queuedIndex,
                                         std::vector<std::shared_ptr<Chunk1>> &chunks,
                                         std::vector<std::shared_ptr<ChunkBuildData>> &chunkBuildDatas,
                                         std::recursive_mutex &mutex,
                                         std::vector<ChunkPackedData> &chunkPackedData,
                                         mcvr::ExternalChunkHandleTable &externalHandles,
                                         uint32_t chunkBuildingBatchSize,
                                         uint32_t chunkBuildingTotalBatches)
    : queuedIndex_(queuedIndex),
      chunks_(chunks),
      chunkBuildDatas_(chunkBuildDatas),
      mutex_(mutex),
      chunkPackedData_(chunkPackedData),
      externalHandles_(externalHandles),
      chunkBuildingBatchSize_(chunkBuildingBatchSize),
      chunkBuildingTotalBatches_(chunkBuildingTotalBatches) {
    auto framework = Renderer::instance().framework();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();
    useSecondaryQueue_ = physicalDevice->mainQueueIndex() == physicalDevice->secondaryQueueIndex();
    auto commandPool = useSecondaryQueue_ ? framework->asyncCommandPool() : framework->mainCommandPool();

    uint32_t numFences = chunkBuildingTotalBatches_;
    for (int i = 0; i < numFences; i++) {
        freeFences_.push(vk::Fence::create(device));
        freeCommandBuffers_.push(vk::CommandBuffer::create(device, commandPool));
    }
}

void ChunkBuildScheduler::tryCheckBatchesFinish() {
    auto framework = Renderer::instance().framework();
    auto device = framework->device();

    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto iterFence = buildingFences_.begin();
    auto iterCommandBuffer = buildingCommandBuffers_.begin();
    auto iterBatch = buildingBatches_.begin();
    for (; iterFence != buildingFences_.end() && iterCommandBuffer != buildingCommandBuffers_.end() &&
           iterBatch != buildingBatches_.end();) {
        const VkResult waitResult = vkWaitForFences(device->vkDevice(), 1, &(*iterFence)->vkFence(), true, 0);
        if (waitResult == VK_SUCCESS) {
            if (chunkPerformanceEnabled) {
                chunkPerformance.batchCompleteCount.fetch_add(1, std::memory_order_relaxed);
            }
            if (chunkPerformanceEnabled && (*iterBatch)->submittedAt.time_since_epoch().count() != 0) {
                chunkPerformance.batchQueueGpuWallNs.fetch_add(
                    elapsedNs((*iterBatch)->submittedAt), std::memory_order_relaxed);
            }
            const VkResult resetFenceResult = vkResetFences(device->vkDevice(), 1, &(*iterFence)->vkFence());
            if (resetFenceResult != VK_SUCCESS) {
                framework->recordFailure(resetFenceResult, "vkResetFences(chunk poll)");
                return;
            }
            const VkResult resetCommandResult = vkResetCommandBuffer((*iterCommandBuffer)->vkCommandBuffer(), 0);
            if (resetCommandResult != VK_SUCCESS) {
                framework->recordFailure(resetCommandResult, "vkResetCommandBuffer(chunk poll)");
                return;
            }
            freeFences_.push(*iterFence);
            freeCommandBuffers_.push(*iterCommandBuffer);

            for (auto chunkBuildData : (*iterBatch)->batchData) {
                mcvr::chunkTrace::note("gpu-complete",chunkBuildData->id,chunkBuildData->version,0,chunkBuildData->slotGeneration);
                if (!externalHandles_.isCurrent(static_cast<uint32_t>(chunkBuildData->id),
                                                chunkBuildData->slotGeneration)) {
                    releaseChunkBuildDataStaging(chunkBuildData);
                    continue;
                }
                bool wasEnqueued = chunks_[chunkBuildData->id]->enqueue(chunkBuildData);
                releaseChunkBuildDataStaging(chunkBuildData);
                if (!wasEnqueued) {
                    continue;
                }

                storeChunkPackedData(
                    chunkPackedData_, chunkBuildData->id, chunkBuildData->x, chunkBuildData->y, chunkBuildData->z,
                    chunkBuildData->geometryCount, chunkBuildData->lightCount,
                    chunkBuildData->lightBuffer != nullptr ? chunkBuildData->lightBuffer->bufferAddress() : 0);
            }

            iterFence = buildingFences_.erase(iterFence);
            iterCommandBuffer = buildingCommandBuffers_.erase(iterCommandBuffer);
            iterBatch = buildingBatches_.erase(iterBatch);
        } else if (waitResult == VK_TIMEOUT || waitResult == VK_NOT_READY) {
            ++iterFence;
            ++iterCommandBuffer;
            ++iterBatch;
        } else {
            framework->recordFailure(waitResult, "vkWaitForFences(chunk poll)");
            return;
        }
    }
}

void ChunkBuildScheduler::waitAllBatchesFinish() {
    auto framework = Renderer::instance().framework();
    auto device = framework->device();

    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto iterFence = buildingFences_.begin();
    auto iterCommandBuffer = buildingCommandBuffers_.begin();
    auto iterBatch = buildingBatches_.begin();
    for (; iterFence != buildingFences_.end() && iterCommandBuffer != buildingCommandBuffers_.end() &&
           iterBatch != buildingBatches_.end();) {
        const VkResult waitResult =
            vkWaitForFences(device->vkDevice(), 1, &(*iterFence)->vkFence(), true, UINT64_MAX);
        if (waitResult == VK_SUCCESS) {
            const VkResult resetFenceResult = vkResetFences(device->vkDevice(), 1, &(*iterFence)->vkFence());
            if (resetFenceResult != VK_SUCCESS) {
                framework->recordFailure(resetFenceResult, "vkResetFences(chunk wait)");
                return;
            }
            const VkResult resetCommandResult = vkResetCommandBuffer((*iterCommandBuffer)->vkCommandBuffer(), 0);
            if (resetCommandResult != VK_SUCCESS) {
                framework->recordFailure(resetCommandResult, "vkResetCommandBuffer(chunk wait)");
                return;
            }
            freeFences_.push(*iterFence);
            freeCommandBuffers_.push(*iterCommandBuffer);

            for (auto chunkBuildData : (*iterBatch)->batchData) {
                mcvr::chunkTrace::note("gpu-complete",chunkBuildData->id,chunkBuildData->version,0,chunkBuildData->slotGeneration);
                if (!externalHandles_.isCurrent(static_cast<uint32_t>(chunkBuildData->id),
                                                chunkBuildData->slotGeneration)) {
                    releaseChunkBuildDataStaging(chunkBuildData);
                    continue;
                }
                bool wasEnqueued = chunks_[chunkBuildData->id]->enqueue(chunkBuildData);
                releaseChunkBuildDataStaging(chunkBuildData);
                if (!wasEnqueued) {
                    continue;
                }

                storeChunkPackedData(
                    chunkPackedData_, chunkBuildData->id, chunkBuildData->x, chunkBuildData->y, chunkBuildData->z,
                    chunkBuildData->geometryCount, chunkBuildData->lightCount,
                    chunkBuildData->lightBuffer != nullptr ? chunkBuildData->lightBuffer->bufferAddress() : 0);
            }

            iterFence = buildingFences_.erase(iterFence);
            iterCommandBuffer = buildingCommandBuffers_.erase(iterCommandBuffer);
            iterBatch = buildingBatches_.erase(iterBatch);
        } else {
            framework->recordFailure(waitResult, "vkWaitForFences(chunk wait)");
            return;
        }
    }


}

void ChunkBuildScheduler::tryScheduleBatches(uint32_t maxBatchSize) {
    if (!Renderer::instance().framework()->isRunning()) return;
    uint32_t submittedThisFrame = 0;
    uint64_t submittedBytes = 0;
    const auto roundStart = mcvr::chunkScheduling::Clock::now();
    while (mcvr::chunkScheduling::withinBudget(submittedThisFrame, submittedBytes,
               mcvr::chunkScheduling::Clock::now()-roundStart)) {
        std::shared_ptr<vk::Fence> fence;
        std::shared_ptr<vk::CommandBuffer> commandBuffer;
        std::shared_ptr<ChunkBuildDataBatch> chunkBuildDataBatch;
        {
            std::unique_lock<std::recursive_mutex> lock(mutex_);
            if (freeFences_.empty() || freeCommandBuffers_.empty()) return;
            // Reject stale work before packing or reserving GPU storage. A slot generation and
            // its latest requested mesh revision must both still match.
            for (auto it=queuedIndex_.begin();it!=queuedIndex_.end();) {
                auto &data=chunkBuildDatas_[*it];
                if (!data || !externalHandles_.isCurrent(uint32_t(*it),data->slotGeneration)
                    || data->version!=chunks_[*it]->desiredVersion) {
                    data=nullptr;it=queuedIndex_.erase(it);
                } else ++it;
            }
            if (queuedIndex_.empty()) return;
            bool interactive=false;
            auto oldest=mcvr::chunkScheduling::Clock::now();
            for (auto id:queuedIndex_) {
                interactive |= chunkBuildDatas_[id]->priority>0;
                oldest=std::min(oldest,chunkBuildDatas_[id]->queuedAt);
            }
            if (!mcvr::chunkScheduling::ready(interactive,uint32_t(queuedIndex_.size()),maxBatchSize,
                    mcvr::chunkScheduling::Clock::now()-oldest)) return;
            // Small interactive batches do not wait to fill. Weighted admission keeps old
            // background owners progressing without placing the entire old queue before interaction.
            uint64_t inFlightBytes = 0;
            for (const auto &batch : buildingBatches_) for (const auto &data : batch->batchData)
                inFlightBytes += uint64_t(data->allVertexCount)*sizeof(vk::VertexFormat::PBRVertex)
                    + uint64_t(data->allIndexCount)*sizeof(uint32_t)
                    + uint64_t(data->lightCount)*sizeof(LightInfo);
            chunkBuildDataBatch=ChunkBuildDataBatch::create(interactive?std::min(4u,maxBatchSize):maxBatchSize,
                queuedIndex_,chunks_,chunkBuildDatas_,Renderer::instance().world()->getCameraPos(),inFlightBytes,admissionSequence_);
            if (chunkBuildDataBatch->batchData.empty()) return;
            admissionSequence_ += chunkBuildDataBatch->batchData.size();
            for (const auto &data:chunkBuildDataBatch->batchData)
                submittedBytes+=uint64_t(data->allVertexCount)*sizeof(vk::VertexFormat::PBRVertex)
                    +uint64_t(data->allIndexCount)*sizeof(uint32_t)
                    +uint64_t(data->lightCount)*sizeof(LightInfo);
            fence=freeFences_.front();freeFences_.pop();
            commandBuffer=freeCommandBuffers_.front();freeCommandBuffers_.pop();
        }

        const auto batchBuildStart = std::chrono::steady_clock::now();
        // Admission includes CPU batch assembly as well as GPU submission. This keeps a burst of
        // light-only or otherwise upload-free batches from consuming an unbounded frame budget.
        submittedThisFrame++;
        for (const auto& data:chunkBuildDataBatch->batchData)
            mcvr::chunkTrace::note("build-prepare",data->id,data->version,0,data->slotGeneration);
        chunkBuildDataBatch->build();
        if (chunkPerformanceEnabled) {
            chunkPerformance.batchBuildCount.fetch_add(1, std::memory_order_relaxed);
            chunkPerformance.batchBuildCpuNs.fetch_add(elapsedNs(batchBuildStart),
                                                        std::memory_order_relaxed);
        }

        bool hasLightUploads = false;
        for (const auto &chunkBuildData : chunkBuildDataBatch->batchData) {
            if (chunkBuildData->lightBuffer != nullptr) {
                hasLightUploads = true;
                break;
            }
        }

        if (chunkBuildDataBatch->blasBatchBuilder == nullptr && !hasLightUploads) {
            std::unique_lock<std::recursive_mutex> lock(mutex_);
            freeFences_.push(fence);
            freeCommandBuffers_.push(commandBuffer);

            for (auto &chunkBuildData : chunkBuildDataBatch->batchData) {
                if (!externalHandles_.isCurrent(static_cast<uint32_t>(chunkBuildData->id),
                                                chunkBuildData->slotGeneration)) {
                    continue;
                }
                if (!chunks_[chunkBuildData->id]->enqueue(chunkBuildData)) {
                    continue;
                }

                storeChunkPackedData(
                    chunkPackedData_, chunkBuildData->id, chunkBuildData->x, chunkBuildData->y, chunkBuildData->z,
                    chunkBuildData->geometryCount, chunkBuildData->lightCount,
                    chunkBuildData->lightBuffer != nullptr ? chunkBuildData->lightBuffer->bufferAddress() : 0);
            }
            continue;
        }

        auto framework = Renderer::instance().framework();
        auto device = framework->device();
        auto physicalDevice = framework->physicalDevice();
        const auto queueFamilyIndex =
            useSecondaryQueue_ ? physicalDevice->secondaryQueueIndex() : physicalDevice->mainQueueIndex();

        commandBuffer->begin();
        if (chunkBuildDataBatch->positionBuffer != nullptr) {
            chunkBuildDataBatch->indexBuffer->uploadToBuffer(commandBuffer);
            chunkBuildDataBatch->positionBuffer->uploadToBuffer(commandBuffer);
            chunkBuildDataBatch->materialBuffer->uploadToBuffer(commandBuffer);
        }
        for (const auto &chunkBuildData : chunkBuildDataBatch->batchData) {
            if (chunkBuildData->lightBuffer != nullptr) {
                chunkBuildData->lightBuffer->uploadToBuffer(commandBuffer);
            }
        }

        std::vector<vk::CommandBuffer::BufferMemoryBarrier> geometryBarriers;
        if (chunkBuildDataBatch->positionBuffer != nullptr) {
            geometryBarriers = {
                {
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                    VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .srcQueueFamilyIndex = queueFamilyIndex,
                    .dstQueueFamilyIndex = queueFamilyIndex,
                    .buffer = chunkBuildDataBatch->indexBuffer,
                },
                {
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                    VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .srcQueueFamilyIndex = queueFamilyIndex,
                    .dstQueueFamilyIndex = queueFamilyIndex,
                    .buffer = chunkBuildDataBatch->positionBuffer,
                },
                {
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .srcQueueFamilyIndex = queueFamilyIndex,
                    .dstQueueFamilyIndex = queueFamilyIndex,
                    .buffer = chunkBuildDataBatch->materialBuffer,
                },
            };
            commandBuffer->barriersBufferImage(geometryBarriers, {});
        }

        std::vector<vk::CommandBuffer::BufferMemoryBarrier> lightBarriers;
        for (const auto &chunkBuildData : chunkBuildDataBatch->batchData) {
            if (chunkBuildData->lightBuffer == nullptr) {
                continue;
            }

            lightBarriers.push_back({
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                .srcQueueFamilyIndex = queueFamilyIndex,
                .dstQueueFamilyIndex = queueFamilyIndex,
                .buffer = chunkBuildData->lightBuffer,
            });
        }
        if (!lightBarriers.empty()) {
            commandBuffer->barriersBufferImage(lightBarriers, {});
        }

        if (chunkBuildDataBatch->blasBatchBuilder != nullptr) {
            device->checkpoint(commandBuffer->vkCommandBuffer(), "chunk.BLAS.build");
            chunkBuildDataBatch->blasBatchBuilder->submit(commandBuffer);
        }
        device->checkpoint(commandBuffer->vkCommandBuffer(), "chunk.batch.end");
        commandBuffer->end();

        VkSubmitInfo vkSubmitInfo = {};
        vkSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        vkSubmitInfo.commandBufferCount = 1;
        vkSubmitInfo.pCommandBuffers = &commandBuffer->vkCommandBuffer();

        const auto submitStart = std::chrono::steady_clock::now();
        const VkResult submitResult =
            useSecondaryQueue_ ? vkQueueSubmit(device->secondaryQueue(), 1, &vkSubmitInfo, fence->vkFence()) :
                                 vkQueueSubmit(device->mainVkQueue(), 1, &vkSubmitInfo, fence->vkFence());
        mcvr::diagnostics::device_loss::note("chunk-queue-submit", submitResult,
            static_cast<uint32_t>(chunkBuildDataBatch->batchData.size()));
        if (chunkPerformanceEnabled) {
            chunkPerformance.batchSubmitCount.fetch_add(1, std::memory_order_relaxed);
            chunkPerformance.batchSubmitCpuNs.fetch_add(elapsedNs(submitStart),
                                                         std::memory_order_relaxed);
        }
        if (submitResult != VK_SUCCESS) {
            framework->recordFailure(submitResult, "vkQueueSubmit(chunk build)");
            commandBuffer->reset();
            std::unique_lock<std::recursive_mutex> lock(mutex_);
            freeFences_.push(fence);
            freeCommandBuffers_.push(commandBuffer);
            for (const auto &chunkBuildData : chunkBuildDataBatch->batchData) {
                releaseChunkBuildDataStaging(chunkBuildData);
            }
            return;
        }

        for (const auto &data:chunkBuildDataBatch->batchData)
            mcvr::chunkTrace::note("build-submit",data->id,data->version,0,data->slotGeneration);
        if (mcvr::chunkTrace::enabled) {
            uint64_t bytes = 0;
            for (const auto &buffer : {chunkBuildDataBatch->positionBuffer, chunkBuildDataBatch->materialBuffer,
                                      chunkBuildDataBatch->indexBuffer})
                if (buffer) bytes += buffer->size();
            for (const auto &data : chunkBuildDataBatch->batchData)
                if (data->lightBuffer) bytes += data->lightBuffer->size();
            // Payload buffers submitted by this chunk batch; excludes AS scratch/output, TLAS and SDK memory.
            mcvr::chunkTrace::note("upload-bytes",int64_t(bytes),int64_t(chunkBuildDataBatch->batchData.size()));
        }
        if (chunkPerformanceEnabled) chunkBuildDataBatch->submittedAt = std::chrono::steady_clock::now();
        std::unique_lock<std::recursive_mutex> lock(mutex_);
        buildingFences_.push_back(fence);
        buildingCommandBuffers_.push_back(commandBuffer);
        buildingBatches_.push_back(chunkBuildDataBatch);
    }
}

uint32_t ChunkBuildScheduler::chunkBuildingBatchSize() {
    return chunkBuildingBatchSize_;
}

uint32_t ChunkBuildScheduler::chunkBuildingTotalBatches() {
    return chunkBuildingTotalBatches_;
}

size_t ChunkBuildScheduler::pendingBuildCount() const {
    return 0; // Unsubmitted requests stay in queuedIndex_; there is no hidden partial batch.
}

size_t ChunkBuildScheduler::activeBatchCount() const {
    return buildingBatches_.size();
}

float Chunk1::buildFactor(std::chrono::steady_clock::time_point currentTime, glm::vec3 cameraPos, glm::vec3 chunkPos) {
    double tDiff = std::chrono::duration<double, std::milli>(currentTime - lastUpdate).count();
    double dDiff = glm::distance(cameraPos, chunkPos);

    double tScore = 1 - exp(-tDiff / T_HALF);
    double nearScore = 1 / (1 + pow(dDiff / D_HALF, D_SENSITIVITY));
    double dScore = D_PRIORITY_FLOOR + (1.0 - D_PRIORITY_FLOOR) * nearScore;
    double score = pow(tScore, T_WEIGHT) * pow(dScore, D_WEIGHT);

    return score;
}

bool Chunk1::enqueue(std::shared_ptr<ChunkBuildData> chunkBuildData) {
    auto framework = Renderer::instance().framework();
    auto &frr = framework->frameResourceRetainer();

    lastUpdate = std::chrono::steady_clock::now();

    if (chunkBuildData->version == desiredVersion && chunkBuildData->version > blasVersion) {
        blasVersion = chunkBuildData->version;
        mcvr::chunkTrace::note("publish",chunkBuildData->id,blasVersion,0,chunkBuildData->slotGeneration);
        x = chunkBuildData->x;
        y = chunkBuildData->y;
        z = chunkBuildData->z;

        frr.retain(blas);
        blas = chunkBuildData->blas;

        frr.retain(indexBufferAddresses);
        indexBufferAddresses =
            std::make_shared<std::vector<VkDeviceAddress>>(std::move(chunkBuildData->indexBufferAddresses));

        frr.retain(positionBufferAddresses);
        positionBufferAddresses =
            std::make_shared<std::vector<VkDeviceAddress>>(std::move(chunkBuildData->positionBufferAddresses));

        frr.retain(materialBufferAddresses);
        materialBufferAddresses =
            std::make_shared<std::vector<VkDeviceAddress>>(std::move(chunkBuildData->materialBufferAddresses));

        frr.retain(indexBuffer);
        indexBuffer = chunkBuildData->indexBuffer;

        frr.retain(positionBuffer);
        positionBuffer = chunkBuildData->positionBuffer;

        frr.retain(materialBuffer);
        materialBuffer = chunkBuildData->materialBuffer;

        frr.retain(lightInfos);
        lightInfos = std::make_shared<std::vector<LightInfo>>(std::move(chunkBuildData->lightInfos));

        frr.retain(lightBuffer);
        lightBuffer = chunkBuildData->lightBuffer;
        lightCount = chunkBuildData->lightCount;

        geometryCount = chunkBuildData->geometryCount;

        frr.retain(geometryGroupNames);
        geometryGroupNames = std::make_shared<std::vector<std::string>>(std::move(chunkBuildData->geometryGroupNames));

        frr.retain(geometryMaterialFlags);
        geometryMaterialFlags =
            std::make_shared<std::vector<uint32_t>>(std::move(chunkBuildData->geometryMaterialFlags));
        return true;
    } else {
        frr.retain(chunkBuildData->blas);
        frr.retain(chunkBuildData->indexBuffer);
        frr.retain(chunkBuildData->positionBuffer);
        frr.retain(chunkBuildData->materialBuffer);
        frr.retain(chunkBuildData->lightBuffer);
        return false;
    }
}

void Chunk1::markDirty(int64_t generation) {
    if (generation >= 0) {
        desiredVersion = std::max(desiredVersion, generation);
        latestVersion = std::max(latestVersion, generation + 1);
    }
    lastUpdate = std::chrono::steady_clock::now();
}

void Chunk1::invalidate(int64_t generation) {
    auto framework = Renderer::instance().framework();
    auto &frr = framework->frameResourceRetainer();

    lastUpdate = std::chrono::steady_clock::now();

    if (generation >= 0) {
        markDirty(generation);
    } else {
        blasVersion = latestVersion++;
        desiredVersion = blasVersion;
    }

    frr.retain(blas);
    blas = nullptr;

    frr.retain(indexBufferAddresses);
    indexBufferAddresses = nullptr;

    frr.retain(positionBufferAddresses);
    positionBufferAddresses = nullptr;

    frr.retain(materialBufferAddresses);
    materialBufferAddresses = nullptr;

    frr.retain(indexBuffer);
    indexBuffer = nullptr;

    frr.retain(positionBuffer);
    positionBuffer = nullptr;

    frr.retain(materialBuffer);
    materialBuffer = nullptr;

    frr.retain(lightInfos);
    lightInfos = nullptr;

    frr.retain(lightBuffer);
    lightBuffer = nullptr;
    lightCount = 0;

    geometryCount = 0;

    frr.retain(geometryGroupNames);
    geometryGroupNames = nullptr;

    frr.retain(geometryMaterialFlags);
    geometryMaterialFlags = nullptr;
}

void Chunk1::retainResources(FrameResourceRetainer &frr) {
    frr.retain(blas);
    frr.retain(indexBufferAddresses);
    frr.retain(positionBufferAddresses);
    frr.retain(materialBufferAddresses);
    frr.retain(indexBuffer);
    frr.retain(positionBuffer);
    frr.retain(materialBuffer);
    frr.retain(lightInfos);
    frr.retain(lightBuffer);
}

void Chunk1::releaseEmissionResources(FrameResourceRetainer &frr) {
    frr.retain(lightInfos);
    lightInfos = nullptr;

    frr.retain(lightBuffer);
    lightBuffer = nullptr;

    lightCount = 0;
}

std::shared_ptr<ChunkRenderData> Chunk1::tryGetValid() {
    auto ret = ChunkRenderData::create();
    ret->x = x;
    ret->y = y;
    ret->z = z;
    ret->blas = blas;
    ret->indexBufferAddresses = indexBufferAddresses;
    ret->positionBufferAddresses = positionBufferAddresses;
    ret->materialBufferAddresses = materialBufferAddresses;
    ret->indexBuffer = indexBuffer;
    ret->positionBuffer = positionBuffer;
    ret->materialBuffer = materialBuffer;
    ret->lightInfos = lightInfos;
    ret->lightBuffer = lightBuffer;
    ret->lightCount = lightCount;
    ret->geometryCount = geometryCount;
    ret->geometryGroupNames = geometryGroupNames;
    ret->geometryMaterialFlags = geometryMaterialFlags;

    return ret;
}

Chunks::Chunks(std::shared_ptr<Framework> framework) {
    importantBLASBuilders_ = std::make_shared<std::vector<std::shared_ptr<vk::BLASBuilder>>>();
}

void Chunks::allocateChunkPackedDataBuffers() {
    auto framework = Renderer::instance().framework();
    auto device = framework->device();
    auto vma = framework->vma();

    chunkPackedDataBuffers_.clear();
    chunkPackedDataBuffers_.resize(framework->swapchain()->imageCount());
    if (chunkPackedData_.empty()) {
        return;
    }

    const size_t chunkPackedDataBytes = chunkPackedData_.size() * sizeof(ChunkPackedData);
    for (auto &chunkPackedDataBuffer : chunkPackedDataBuffers_) {
        chunkPackedDataBuffer = vk::DeviceLocalBuffer::create(vma, device, false, chunkPackedDataBytes,
                                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
}

void Chunks::releaseEmissionResources() {
    auto framework = Renderer::instance().framework();
    auto &frr = framework->frameResourceRetainer();

    for (auto &chunk : chunks_) {
        if (chunk != nullptr) {
            chunk->releaseEmissionResources(frr);
        }
    }

    for (auto &chunkBuildData : chunkBuildDatas_) {
        if (chunkBuildData == nullptr) {
            continue;
        }
        chunkBuildData->lightInfos.clear();
        frr.retain(chunkBuildData->lightBuffer);
        chunkBuildData->lightBuffer = nullptr;
        chunkBuildData->lightCount = 0;
    }

    std::fill(chunkPackedData_.begin(), chunkPackedData_.end(), ChunkPackedData{});
    for (auto &chunkPackedDataBuffer : chunkPackedDataBuffers_) {
        frr.retain(chunkPackedDataBuffer);
        chunkPackedDataBuffer = nullptr;
    }
    chunkPackedDataBuffers_.clear();
}

void Chunks::reset(uint32_t numChunks,
                   uint32_t sizeX,
                   uint32_t sizeY,
                   uint32_t sizeZ,
                   int32_t bottomSectionCoord) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);

    auto framework = Renderer::instance().framework();
    auto device = framework->device();
    if (framework->waitRenderQueueIdle() != VK_SUCCESS || framework->waitBackendQueueIdle() != VK_SUCCESS) { return; }

    sizeX_ = static_cast<int32_t>(sizeX);
    sizeY_ = static_cast<int32_t>(sizeY);
    sizeZ_ = static_cast<int32_t>(sizeZ);
    bottomSectionCoord_ = bottomSectionCoord;
    primaryChunkCount_ = numChunks;
    externalHandles_.reset(numChunks);
    chunkStorageSectionPos_ = glm::ivec3(0, bottomSectionCoord, 0);

    importantBLASBuilders_ = std::make_shared<std::vector<std::shared_ptr<vk::BLASBuilder>>>();

    chunks_.clear();
    chunks_.resize(numChunks);
    chunkPackedData_.assign(numChunks, ChunkPackedData{});
    chunkPackedDataBuffers_.clear();
    if (Renderer::options.collectChunkEmission) {
        allocateChunkPackedDataBuffers();
    }
    chunkBuildDatas_.clear();
    chunkBuildDatas_.resize(numChunks);
    queuedIndex_.clear();

    for (int i = 0; i < numChunks; i++) {
        chunks_[i] = Chunk1::create();
        chunkBuildDatas_[i] = nullptr;
    }

    uint32_t chunkBuildingBatchSize = Renderer::instance().options.chunkBuildingBatchSize;
    uint32_t chunkBuildingTotalBatches = Renderer::instance().options.chunkBuildingTotalBatches;
    chunkBuildScheduler_ =
        ChunkBuildScheduler::create(queuedIndex_, chunks_, chunkBuildDatas_, mutex_, chunkPackedData_, externalHandles_,
                                    chunkBuildingBatchSize, chunkBuildingTotalBatches);
}

void Chunks::resetScheduler() {
    std::unique_lock<std::recursive_mutex> lock(mutex_);

    if (chunkBuildScheduler_ == nullptr) return;

    mcvr::failure::runCheckedStage([&] { chunkBuildScheduler_->waitAllBatchesFinish(); });

    uint32_t chunkBuildingBatchSize = Renderer::instance().options.chunkBuildingBatchSize;
    uint32_t chunkBuildingTotalBatches = Renderer::instance().options.chunkBuildingTotalBatches;
    chunkBuildScheduler_ =
        ChunkBuildScheduler::create(queuedIndex_, chunks_, chunkBuildDatas_, mutex_, chunkPackedData_, externalHandles_,
                                    chunkBuildingBatchSize, chunkBuildingTotalBatches);
}

void Chunks::setCollectChunkEmission(bool collect) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);

    if (chunkBuildScheduler_ != nullptr) {
        mcvr::failure::runCheckedStage([&] { chunkBuildScheduler_->waitAllBatchesFinish(); });
    }

    auto textures = Renderer::instance().textures();
    auto emission = textures != nullptr ? textures->emission() : nullptr;
    for (auto &chunkBuildData : chunkBuildDatas_) {
        if (chunkBuildData == nullptr) {
            continue;
        }

        chunkBuildData->collectChunkEmission = collect;
        if (collect && emission != nullptr) {
            chunkBuildData->buildLightInfos(*emission);
        } else {
            chunkBuildData->lightInfos.clear();
            chunkBuildData->lightCount = 0;
            chunkBuildData->lightBuffer = nullptr;
        }
    }

    if (collect) {
        if (chunkPackedDataBuffers_.empty()) {
            allocateChunkPackedDataBuffers();
        }
        return;
    }

    releaseEmissionResources();
}

void Chunks::resetFrame() {
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto framework = Renderer::instance().framework();
    auto &frr = framework->frameResourceRetainer();

    frr.retain(importantBLASBuilders_);
    importantBLASBuilders_ = std::make_shared<std::vector<std::shared_ptr<vk::BLASBuilder>>>();

    if (Renderer::options.collectChunkEmission && !chunkPackedData_.empty() &&
        context->frameIndex < chunkPackedDataBuffers_.size()) {
        auto &chunkPackedDataBuffer = chunkPackedDataBuffers_[context->frameIndex];
        if (chunkPackedDataBuffer != nullptr) {
            const size_t chunkPackedDataBytes = chunkPackedData_.size() * sizeof(ChunkPackedData);
            chunkPackedDataBuffer->uploadToStagingBuffer(chunkPackedData_.data(), chunkPackedDataBytes, 0);
            Renderer::instance().buffers()->queueImportantWorldUpload(chunkPackedDataBuffer);
        }
    }
}

void Chunks::markChunkDirty(int64_t handle, int64_t generation) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto resolved = externalHandles_.resolve(handle);
    if (!resolved) return;
    const uint32_t id = *resolved;
    chunks_[id]->markDirty(generation);
    if (chunkBuildDatas_[id] != nullptr && chunkBuildDatas_[id]->version < generation) {
        auto &frr = Renderer::instance().framework()->frameResourceRetainer();
        frr.retain(chunkBuildDatas_[id]);
        chunkBuildDatas_[id] = nullptr;
        queuedIndex_.erase(id);
    }
}

void Chunks::invalidateChunk(int64_t handle, int64_t generation) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto resolved = externalHandles_.resolve(handle);
    if (!resolved) return;
    const uint32_t id = *resolved;
    auto framework = Renderer::instance().framework();
    auto &frr = framework->frameResourceRetainer();

    queuedIndex_.erase(id);

    frr.retain(chunkBuildDatas_[id]);
    chunkBuildDatas_[id] = nullptr;

    chunks_[id]->invalidate(generation);

    storeChunkPackedData(chunkPackedData_, id, chunks_[id]->x, chunks_[id]->y, chunks_[id]->z, 0, 0, 0);
}

void Chunks::relocateChunk(int64_t handle, int x, int y, int z, int64_t generation) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto resolved = externalHandles_.resolve(handle);
    if (!resolved) return;
    const uint32_t id = *resolved;
    auto framework = Renderer::instance().framework();
    auto &frr = framework->frameResourceRetainer();

    queuedIndex_.erase(id);

    frr.retain(chunkBuildDatas_[id]);
    chunkBuildDatas_[id] = nullptr;

    chunks_[id]->x = x;
    chunks_[id]->y = y;
    chunks_[id]->z = z;
    chunks_[id]->invalidate(generation);

    storeChunkPackedData(chunkPackedData_, id, x, y, z, 0, 0, 0);
}

void Chunks::queueChunkBuild(ChunkBuildTask task) {
    const auto inputStart = std::chrono::steady_clock::now();
    const auto recordInputTiming = [&]() {
        if (!chunkPerformanceEnabled) return;
        chunkPerformance.buildInputCount.fetch_add(1, std::memory_order_relaxed);
        chunkPerformance.buildInputCpuNs.fetch_add(elapsedNs(inputStart), std::memory_order_relaxed);
    };
    uint32_t slotGeneration = 0;
    {
        std::unique_lock<std::recursive_mutex> lock(mutex_);
        auto resolved = externalHandles_.resolve(task.id);
        if (!resolved) return;
        task.id = *resolved;
        slotGeneration = externalHandles_.generation(*resolved);
        if (task.generation >= 0 && task.generation < chunks_[task.id]->desiredVersion) return;
    }

    uint32_t allVertexCount = 0, allIndexCount = 0;
    std::vector<World::GeometryTypes> geometryTypes;
    std::vector<std::string> geometryGroupNames;
    std::vector<uint32_t> geometryMaterialFlags;
    std::vector<std::vector<vk::VertexFormat::PBRVertex>> vertices;
    std::vector<std::vector<uint32_t>> indices;

    for (int i = 0; i < task.geometryCount; i++) {
        World::GeometryTypes geometryType = static_cast<World::GeometryTypes>(task.geometryTypes[i]);
        auto &geometryVertices = vertices.emplace_back();
        auto &geometryIndices = indices.emplace_back();

        geometryVertices.resize(task.vertexCounts[i]);
        std::memcpy(geometryVertices.data(), task.vertices[i],
                    task.vertexCounts[i] * sizeof(vk::VertexFormat::PBRVertex));

        for (int j = 0; j < task.vertexCounts[i]; j += 4) {
            geometryIndices.push_back(j + 0);
            geometryIndices.push_back(j + 1);
            geometryIndices.push_back(j + 2);
            geometryIndices.push_back(j + 2);
            geometryIndices.push_back(j + 3);
            geometryIndices.push_back(j + 0);
        }

        if (geometryVertices.empty() || geometryIndices.empty()) {
            vertices.pop_back();
            indices.pop_back();
            continue;
        }

        geometryTypes.push_back(geometryType);
        if (task.geometryGroupNames != nullptr && task.geometryGroupNames[i] != nullptr) {
            geometryGroupNames.emplace_back(task.geometryGroupNames[i]);
        } else {
            geometryGroupNames.emplace_back("default");
        }
        geometryMaterialFlags.push_back(task.geometryMaterialFlags == nullptr
                                            ? 0u
                                            : static_cast<uint32_t>(task.geometryMaterialFlags[i]));

        allVertexCount += geometryVertices.size();
        allIndexCount += geometryIndices.size();
    }

    std::unique_lock<std::recursive_mutex> lock(mutex_);
    if (!externalHandles_.isCurrent(static_cast<uint32_t>(task.id), slotGeneration)
        || (task.generation >= 0 && task.generation < chunks_[task.id]->desiredVersion)) {
        recordInputTiming();
        return;
    }

    const bool collectChunkEmission = Renderer::options.collectChunkEmission && task.collectEmission;
    const int64_t buildVersion = task.generation >= 0
        ? task.generation
        : chunks_[task.id]->latestVersion++;
    chunks_[task.id]->desiredVersion = std::max(chunks_[task.id]->desiredVersion, buildVersion);
    std::shared_ptr<ChunkBuildData> chunkBuildData =
        ChunkBuildData::create(task.id, task.x, task.y, task.z, buildVersion,
                               collectChunkEmission, allVertexCount, allIndexCount,
                               static_cast<uint32_t>(vertices.size()), std::move(geometryTypes),
                               std::move(geometryGroupNames), std::move(geometryMaterialFlags),
                               std::move(vertices), std::move(indices));
    chunkBuildData->slotGeneration = slotGeneration;
    chunkBuildData->priority = task.priority;
    mcvr::chunkTrace::note("native-enqueue",task.id,buildVersion,0,slotGeneration);

    if (collectChunkEmission && (Renderer::instance().textures() != nullptr)) {
        auto textures = Renderer::instance().textures();
        if (auto emission = textures->emission(); emission != nullptr) {
            chunkBuildData->buildLightInfos(*emission);
        }
    }

    if (task.isImportant) {
        auto &frr = Renderer::instance().framework()->frameResourceRetainer();
        queuedIndex_.erase(task.id);
        frr.retain(chunkBuildDatas_[task.id]);
        chunkBuildDatas_[task.id] = nullptr;

        chunkBuildData->build(false);
        if (chunkBuildData->positionBuffer != nullptr) {
            Renderer::instance().buffers()->queueImportantWorldUpload(chunkBuildData->indexBuffer);
            Renderer::instance().buffers()->queueImportantWorldUpload(chunkBuildData->positionBuffer);
            Renderer::instance().buffers()->queueImportantWorldUpload(chunkBuildData->materialBuffer);
        }
        if (chunkBuildData->lightBuffer != nullptr) {
            Renderer::instance().buffers()->queueImportantWorldUpload(chunkBuildData->lightBuffer);
        }
        if (chunkBuildData->blasBuilder != nullptr) { importantBLASBuilders_->push_back(chunkBuildData->blasBuilder); }

        if (!chunks_[task.id]->enqueue(chunkBuildData)) {
            recordInputTiming();
            return;
        }

        storeChunkPackedData(
            chunkPackedData_, chunkBuildData->id, chunkBuildData->x, chunkBuildData->y, chunkBuildData->z,
            chunkBuildData->geometryCount, chunkBuildData->lightCount,
            chunkBuildData->lightBuffer != nullptr ? chunkBuildData->lightBuffer->bufferAddress() : 0);
    } else {
        queuedIndex_.insert(task.id);
        chunkBuildDatas_[task.id] = chunkBuildData;
    }
    recordInputTiming();
}

int64_t Chunks::allocateExternalChunk() {
    std::unique_lock<std::recursive_mutex> lock(mutex_);

    const auto allocation = externalHandles_.allocate(static_cast<uint32_t>(chunks_.size()));
    const uint32_t id = allocation.slot;
    if (allocation.reused) {
        chunks_[id] = Chunk1::create();
        chunkBuildDatas_[id] = nullptr;
        chunkPackedData_[id] = ChunkPackedData{};
    } else {
        chunks_.push_back(Chunk1::create());
        chunkBuildDatas_.push_back(nullptr);
        chunkPackedData_.emplace_back();
    }

    if (!allocation.reused && Renderer::options.collectChunkEmission
        && !chunkPackedDataBuffers_.empty()) {
        auto &frr = Renderer::instance().framework()->frameResourceRetainer();
        for (auto &buffer : chunkPackedDataBuffers_) {
            frr.retain(buffer);
        }
        allocateChunkPackedDataBuffers();
    }

    return allocation.handle;
}

void Chunks::updateExternalChunkTransform(int64_t handle, const glm::dmat4 &transform) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto resolved = externalHandles_.resolve(handle);
    if (!resolved || *resolved < primaryChunkCount_) return;
    const uint32_t id = *resolved;

    chunks_[id]->hasCustomTransform = true;
    chunks_[id]->customTransform = transform;
}

void Chunks::releaseExternalChunk(int64_t handle) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto resolved = externalHandles_.resolve(handle);
    if (!resolved || *resolved < primaryChunkCount_) return;
    const uint32_t id = *resolved;

    queuedIndex_.erase(id);
    auto &frr = Renderer::instance().framework()->frameResourceRetainer();
    frr.retain(chunkBuildDatas_[id]);
    chunkBuildDatas_[id] = nullptr;
    chunks_[id]->invalidate();
    chunks_[id]->hasCustomTransform = false;
    chunkPackedData_[id] = ChunkPackedData{};
    externalHandles_.release(handle);
}

bool Chunks::isChunkReady(int64_t handle) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto resolved = externalHandles_.resolve(handle);
    if (!resolved) return false;
    const uint32_t id = *resolved;
    auto chunkRenderData = chunks_[id]->tryGetValid();
    return chunkRenderData->blas != nullptr;
}

uint32_t Chunks::countReadyPrimaryChunks() {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    uint32_t ready = 0;
    const size_t count = std::min(static_cast<size_t>(primaryChunkCount_), chunks_.size());
    for (size_t i = 0; i < count; i++) {
        if (chunks_[i] != nullptr && chunks_[i]->tryGetValid()->blas != nullptr) {
            ready++;
        }
    }
    return ready;
}

std::string Chunks::performanceSnapshot() {
    if (!chunkPerformanceEnabled) return "disabled (set RADIANCE_CHUNK_PERF=1)";
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    auto take = [](std::atomic<uint64_t> &value) {
        return value.exchange(0, std::memory_order_relaxed);
    };
    std::ostringstream result;
    result << "build_input_count=" << take(chunkPerformance.buildInputCount)
           << "; build_input_cpu_ns=" << take(chunkPerformance.buildInputCpuNs)
           << "; batch_build_count=" << take(chunkPerformance.batchBuildCount)
           << "; batch_build_cpu_ns=" << take(chunkPerformance.batchBuildCpuNs)
           << "; batch_submit_count=" << take(chunkPerformance.batchSubmitCount)
           << "; batch_submit_cpu_ns=" << take(chunkPerformance.batchSubmitCpuNs)
           << "; batch_complete_count=" << take(chunkPerformance.batchCompleteCount)
           << "; batch_queue_gpu_wall_ns=" << take(chunkPerformance.batchQueueGpuWallNs)
           << "; queued_builds=" << queuedIndex_.size()
           << "; pending_batch_builds="
           << (chunkBuildScheduler_ == nullptr ? 0 : chunkBuildScheduler_->pendingBuildCount())
           << "; active_gpu_batches="
           << (chunkBuildScheduler_ == nullptr ? 0 : chunkBuildScheduler_->activeBatchCount());
    return result.str();
}

void Chunks::close() {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    if (chunkBuildScheduler_ != nullptr) {
        chunkBuildScheduler_->waitAllBatchesFinish();
        chunkBuildScheduler_ = nullptr;
    }

    queuedIndex_.clear();
    chunkBuildDatas_.clear();
    importantBLASBuilders_ = nullptr;
    chunkPackedData_.clear();
    chunkPackedDataBuffers_.clear();
    chunks_.clear();
    sizeX_ = 0;
    sizeY_ = 0;
    sizeZ_ = 0;
    bottomSectionCoord_ = 0;
    chunkStorageSectionPos_ = glm::ivec3(0);
    primaryChunkCount_ = 0;
    externalHandles_.reset(0);
}

std::recursive_mutex &Chunks::mutex() {
    return mutex_;
}

std::vector<std::shared_ptr<Chunk1>> &Chunks::chunks() {
    return chunks_;
}

std::shared_ptr<ChunkBuildScheduler> Chunks::chunkBuildScheduler() {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    return chunkBuildScheduler_;
}

std::vector<std::shared_ptr<vk::BLASBuilder>> &Chunks::importantBLASBuilders() {
    return *importantBLASBuilders_;
}

std::shared_ptr<vk::DeviceLocalBuffer> Chunks::chunkPackedData() {
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    if (context->frameIndex >= chunkPackedDataBuffers_.size()) {
        return nullptr;
    }
    return chunkPackedDataBuffers_[context->frameIndex];
}

glm::ivec4 Chunks::chunkGridInfo() {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    return glm::ivec4(sizeX_, sizeY_, sizeZ_, bottomSectionCoord_);
}

void Chunks::setChunkStorageSectionPos(glm::ivec3 sectionPos) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    chunkStorageSectionPos_ = sectionPos;
}

glm::ivec4 Chunks::chunkStorageSectionPos() {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    return glm::ivec4(chunkStorageSectionPos_, 0);
}
