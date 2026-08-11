#pragma once

#include "common/shared.hpp"
#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/vulkan/all_core_vulkan.hpp"
#include "core/render/rigid_instances.hpp"
#include "core/render/hit_groups.hpp"

#include <map>
#include <mutex>
#include <queue>
#include <unordered_map>

class Framework;
class FrameworkContext;
class RayTracingModule;
struct RayTracingModuleContext;
struct Entity;
struct Chunk1;
struct RigidInstanceBatch;

struct WorldPrepareContext;

class WorldPrepare : public SharedObject<WorldPrepare> {
    friend RayTracingModule;
    friend RayTracingModuleContext;
    friend WorldPrepareContext;

  public:
    WorldPrepare();

    void init(std::shared_ptr<Framework> framework, std::shared_ptr<RayTracingModule> rayTracingModule);

    void build();
    // GPU idle, client left its world: drop the last frames' TLAS, scene buffers and history, which
    // hold every chunk, entity and Flywheel BLAS of those frames until the next world frame.
    void releaseScene();

  private:
    using EntityRenderDataBatch = std::map<uint64_t, std::pair<std::shared_ptr<Entity>, VkTransformMatrixKHR>>;
    using ChunkTransformBatch =
        std::map<std::shared_ptr<Chunk1>, glm::dmat4, std::owner_less<std::shared_ptr<Chunk1>>>;

    std::weak_ptr<Framework> framework_;
    std::weak_ptr<RayTracingModule> rayTracingModule_;

    std::map<uint32_t, std::queue<EntityRenderDataBatch>> previousEntityRenderDataBatches_;
    EntityRenderDataBatch emptyEntityRenderDataBatch_;
    std::recursive_mutex entityRenderDataBatchesMtx_;

    std::map<uint32_t, std::queue<ChunkTransformBatch>> previousChunkTransformBatches_;
    ChunkTransformBatch emptyChunkTransformBatch_;
    std::recursive_mutex chunkTransformBatchesMtx_;

    using FlywheelInstanceKey = std::pair<uint64_t, uint64_t>;
    std::map<uint32_t, std::map<FlywheelInstanceKey, glm::mat4>> previousFlywheelTransforms_;
    std::recursive_mutex flywheelTransformMtx_;

    // Rigid persistent-model history per view. Each frame retains the instance batch whose model
    // buffers its recorded previous-geometry addresses point into.
    struct RigidHistoryState {
        mcvr::rigid::HistoryFrame frame;
        std::shared_ptr<RigidInstanceBatch> owner;
    };
    std::map<uint32_t, std::queue<std::shared_ptr<RigidHistoryState>>> previousRigidHistory_;

    std::vector<std::shared_ptr<WorldPrepareContext>> contexts_;
};

struct WorldPrepareContext : public SharedObject<WorldPrepareContext> {
    std::weak_ptr<FrameworkContext> frameworkContext;
    std::weak_ptr<RayTracingModuleContext> rayTracingModuleContext;
    std::weak_ptr<WorldPrepare> worldPrepare;

    std::shared_ptr<vk::TLAS> tlas;
    std::shared_ptr<vk::TLASBuilder> tlasBuilder;

    std::shared_ptr<vk::DeviceLocalBuffer> blasOffsetsBuffer;
    std::shared_ptr<vk::DeviceLocalBuffer> indexBufferAddr;
    std::shared_ptr<vk::DeviceLocalBuffer> positionBufferAddr;
    std::shared_ptr<vk::DeviceLocalBuffer> materialBufferAddr;
    std::shared_ptr<vk::DeviceLocalBuffer> lastIndexBufferAddr;
    std::shared_ptr<vk::DeviceLocalBuffer> lastPositionBufferAddr;
    std::shared_ptr<vk::DeviceLocalBuffer> lastObjToWorldMat;
    std::shared_ptr<vk::DeviceLocalBuffer> instanceAppearanceBuffer;
    // Interned hit-group ids, one per instance record plus one per geometry.
    std::vector<mcvr::hitgroups::Id> hitGroups;

    WorldPrepareContext(std::shared_ptr<FrameworkContext> frameworkContext, std::shared_ptr<WorldPrepare> worldprepare);

    void uploadBuffer(std::vector<uint32_t> &blasOffsets,
                      std::vector<uint64_t> &indexBufferAddrs,
                      std::vector<uint64_t> &positionBufferAddrs,
                      std::vector<uint64_t> &materialBufferAddrs,
                      std::vector<uint64_t> &lastIndexBufferAddrs,
                      std::vector<uint64_t> &lastPositionBufferAddrs,
                      std::vector<glm::mat4> &lastObjToWorldMats,
                      std::vector<vk::VertexFormat::InstanceAppearance> &instanceAppearances);
    void setupHitGroupSbt(const std::unordered_map<std::string, uint32_t> &hitGroupNameToIndex,
                          uint32_t fallbackHitGroupIndex,
                          uint32_t shadowHitGroupIndex,
                          std::shared_ptr<vk::CommandBuffer> commandBuffer,
                          std::shared_ptr<vk::SBT> updateSbt,
                          std::shared_ptr<vk::SBT> querySbt);
    void render();
};
