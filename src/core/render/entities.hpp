#pragma once

#include "core/render/world_mesh_audit.hpp"
#include "core/render/submitted_geometry_cache.hpp"

#include "common/shared.hpp"
#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/vulkan/all_core_vulkan.hpp"

#include "core/render/world.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

class Framework;

struct EntitiesBuildTask {
    float lineWidth;
    World::Coordinates coordinate;
    bool normalOffset;
    int entityCount;
    int *entityHashCodes;
    double *entityXs;
    double *entityYs;
    double *entityZs;
    int *entityRayTracingFlags;
    int *entityPostRenderFlags;
    int *entityPrebuiltBLASs;
    int *entityPosts;
    int *entityGeometryCounts;
    /**
     * Optional per-entity orthonormal frame (9 floats per entity: axisX, axisY, axisZ, each in
     * the same space as the submitted vertices). Line extrusion uses these axes instead of the
     * world axes so a rotated owner (for example a Sable sub-level) keeps a stable square-section
     * roll. {@code nullptr} means the world frame.
     */
    const float *entityLineFrames = nullptr;
    int *geometryTypes;
    const char **geometryGroupNames;
    const char **geometryContentNames;
    int *geometryTextures;
    int *vertexFormats;
    int *indexFormats;
    int *vertexCounts;
    void **vertices;
    uint64_t worldToken = 0;
    uint64_t frameToken = 0;
    uint64_t resourceGeneration = 0;
    uint64_t stageToken = 0;
    int stage = 0;
    int *geometryIndexTypes = nullptr;
    int *geometryIndexCounts = nullptr;
    int *geometryIndexByteCounts = nullptr;
    void **geometryIndices = nullptr;
    int *geometryAlphaModes = nullptr;
    float *geometryEmissions = nullptr;
    const char **geometryShaderKeys = nullptr;
    const char **geometryMaterialKeys = nullptr;
};

struct WorldMeshBuildTask {
    uint64_t worldToken;
    uint64_t frameToken;
    uint64_t resourceGeneration;
    uint64_t stageToken;
    int stage;
    World::Coordinates coordinate;
    double originX, originY, originZ;
    int sourceId;
    int rayTracingFlag;
    int geometryType;
    int textureId;
    int vertexFormat;
    int drawMode;
    int indexType;
    int vertexCount;
    int indexCount;
    void *vertices;
    int vertexBytes;
    void *indices;
    int indexBytes;
    int alphaMode;
    float emission;
    const char *shaderKey;
    const char *materialKey;
    uint64_t auditId;
};

struct EntityBuildData : public SharedObject<EntityBuildData> {
    std::vector<uint32_t> geometryMaterialFlags;
    uint64_t auditId = 0;
    int hashCode;
    double x, y, z;
    int rayTracingFlag;
    int postRenderFlag;
    int prebuiltBLAS;
    World::Coordinates coordinate;
    uint32_t geometryCount;
    std::vector<World::GeometryTypes> geometryTypes;
    std::vector<std::string> geometryGroupNames;
    std::vector<std::string> geometryContentNames;
    std::vector<std::vector<vk::VertexFormat::PBRVertex>> vertices;
    std::vector<std::vector<uint32_t>> indices;
    std::vector<uint32_t> emissiveOverlayTextureIDs;
    uint64_t worldToken;
    uint64_t frameToken;
    uint64_t resourceGeneration;
    uint64_t stageToken;
    int worldStage;
    std::vector<std::string> shaderKeys;
    std::vector<std::string> materialKeys;
    std::vector<VkDeviceAddress> indexBufferAddresses;
    std::vector<VkDeviceAddress> positionBufferAddresses;
    std::vector<VkDeviceAddress> materialBufferAddresses;
    std::shared_ptr<vk::BLAS> blas;

    EntityBuildData(int hashCode,
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
                    uint64_t worldToken = 0,
                    uint64_t frameToken = 0,
                    uint64_t resourceGeneration = 0,
                    uint64_t stageToken = 0,
                    int worldStage = 0,
                    std::vector<std::string> &&shaderKeys = {},
                    std::vector<std::string> &&materialKeys = {});
};

struct EntityBuildDataBatch : public SharedObject<EntityBuildDataBatch> {
    std::vector<std::shared_ptr<EntityBuildData>> datas;

    std::shared_ptr<vk::DeviceLocalBuffer> indexBuffer;
    std::shared_ptr<vk::DeviceLocalBuffer> positionBuffer;
    std::shared_ptr<vk::DeviceLocalBuffer> materialBuffer;
    std::shared_ptr<vk::BLASBatchBuilder> blasBatchBuilder;

    void addData(std::shared_ptr<EntityBuildData> data);
    void build();
};

struct EntityPostBuildDataBatch : public SharedObject<EntityPostBuildDataBatch> {
    std::vector<std::shared_ptr<EntityBuildData>> datas;

    void addData(std::shared_ptr<EntityBuildData> data);
};

struct Entity;
struct EntityBatch;
struct EntityPost;
struct EntityPostBatch;

struct Entity : public SharedObject<Entity> {
    glm::mat4 instanceTransform{1.0f};
    uint32_t uiSceneOwner = 0;
    std::vector<uint32_t> geometryMaterialFlags;
    int hashCode;
    double x, y, z;
    int rayTracingFlag;
    int prebuiltBLAS;
    World::Coordinates coordinate;
    uint64_t worldToken;
    uint64_t frameToken;
    uint64_t resourceGeneration;
    uint64_t stageToken;
    int worldStage;
    std::shared_ptr<std::vector<std::string>> shaderKeys;
    std::shared_ptr<std::vector<std::string>> materialKeys;

    std::shared_ptr<vk::BLAS> blas;
    std::shared_ptr<std::vector<VkDeviceAddress>> indexBufferAddresses;
    std::shared_ptr<std::vector<VkDeviceAddress>> positionBufferAddresses;
    std::shared_ptr<std::vector<VkDeviceAddress>> materialBufferAddresses;
    std::shared_ptr<vk::DeviceLocalBuffer> indexBuffer;
    std::shared_ptr<vk::DeviceLocalBuffer> positionBuffer;
    std::shared_ptr<vk::DeviceLocalBuffer> materialBuffer;

    uint32_t geometryCount;
    std::shared_ptr<std::vector<std::string>> geometryGroupNames;
    std::shared_ptr<std::vector<std::string>> geometryContentNames;
    std::shared_ptr<std::vector<uint32_t>> vertexCounts;
    std::shared_ptr<std::vector<uint32_t>> indexCounts;

    Entity(std::shared_ptr<EntityBuildData> entityBuildData);
};

struct EntityBatch : public SharedObject<EntityBatch> {
    EntityBatch() = default;
    std::vector<std::shared_ptr<Entity>> entities;

    std::shared_ptr<vk::DeviceLocalBuffer> indexBuffer;
    std::shared_ptr<vk::DeviceLocalBuffer> positionBuffer;
    std::shared_ptr<vk::DeviceLocalBuffer> materialBuffer;

    EntityBatch(std::shared_ptr<EntityBuildDataBatch> entityBuildDataBatch);
};

struct EntityPost : public SharedObject<EntityPost> {
    int postRenderFlag;
    double x, y, z;

    uint32_t geometryCount;
    std::vector<std::string> geometryContentNames;
    std::vector<uint32_t> indexCounts;

    std::vector<std::shared_ptr<vk::DeviceLocalBuffer>> vertexBuffers;
    std::vector<std::shared_ptr<vk::DeviceLocalBuffer>> indexBuffers;

    EntityPost(std::shared_ptr<EntityBuildData> entityBuildData);
};

struct EntityPostBatch : public SharedObject<EntityPostBatch> {
    std::vector<std::shared_ptr<EntityPost>> entities;

    EntityPostBatch(std::shared_ptr<EntityPostBuildDataBatch> entityPostBuildDataBatch);
};

class Entities : public SharedObject<Entities> {
    friend World;

  public:
    Entities(std::shared_ptr<Framework> framework);

    void resetFrame();
    void queueBuild(EntitiesBuildTask task);
    bool beginCachedCloud(uint64_t revision, double x, double y, double z);
    void endCachedCloud(bool success);
    void recordCachedCloudBuild(const std::shared_ptr<vk::CommandBuffer> &commands);
    void commitCachedCloudBuild();
    int beginWorldMeshFrame(uint64_t worldToken, uint64_t frameToken, uint64_t resourceGeneration);
    int beginWorldMeshStage(uint64_t worldToken, uint64_t frameToken, uint64_t resourceGeneration,
                            uint64_t stageToken, int stage);
    int queueWorldMesh(const WorldMeshBuildTask &task);
    int pollWorldMeshAudit(uint64_t auditId, bool consumeTerminal);
    void endWorldMeshStage(uint64_t worldToken, uint64_t frameToken, uint64_t resourceGeneration,
                           uint64_t stageToken, bool commit);
    void endWorldMeshFrame(uint64_t worldToken, uint64_t frameToken, uint64_t resourceGeneration,
                           bool commit);
    void invalidateWorldMeshGeneration(uint64_t resourceGeneration);
    bool acceptsWorldMeshGeneration(uint64_t worldToken, uint64_t frameToken,
                                    uint64_t resourceGeneration) const;
    void build();
    void close();
    std::shared_ptr<EntityBatch> entityBatch();
    void publishCachedEntities(std::vector<std::shared_ptr<Entity>> entities);
    // UI triangle captures may animate material data without changing topology.
    std::shared_ptr<Entity> buildUiMaterialUpdate(const std::shared_ptr<Entity> &previous);
    std::shared_ptr<EntityPostBatch> entityPostBatch();
    std::shared_ptr<vk::BLASBatchBuilder> blasBatchBuilder();

  private:
    std::shared_ptr<EntityBatch> entityBatch_;
    std::shared_ptr<EntityPostBatch> entityPostBatch_;
    std::shared_ptr<EntityBuildDataBatch> entityBuildDataBatch_;
    std::shared_ptr<EntityPostBuildDataBatch> entityPostBuildDataBatch_;

    std::shared_ptr<vk::BLASBatchBuilder> blasBatchBuilder_;
    mcvr::SubmittedGeometryCache<EntityBatch> cloudCache_;
    std::shared_ptr<EntityBuildDataBatch> cloudBuildData_;
    std::shared_ptr<vk::BLASBatchBuilder> cloudBlasBuilder_;
    std::vector<std::shared_ptr<Entity>> queuedClouds_;
    uint64_t cloudRevision_ = 0;
    size_t cloudCaptureStart_ = 0;
    bool cloudCapturing_ = false, cloudBuildRecorded_ = false;

    struct StageCheckpoint {
        uint64_t token;
        size_t entityCount;
        size_t postCount;
    };
    uint64_t activeWorldToken_ = 0;
    uint64_t activeFrameToken_ = 0;
    uint64_t activeResourceGeneration_ = 0;
    size_t frameEntityCheckpoint_ = 0;
    size_t framePostCheckpoint_ = 0;
    bool worldMeshFrameOpen_ = false;
    std::vector<StageCheckpoint> worldMeshStages_;
    std::unordered_map<uint64_t, WorldMeshAuditState> worldMeshAuditStates_;
};
