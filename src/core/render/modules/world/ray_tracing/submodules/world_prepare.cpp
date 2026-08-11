#include "core/render/chunk_trace.hpp"
#include "core/logging.hpp"
#include "core/diagnostics/frame_profile.hpp"
#include "core/render/material_faces.hpp"
#include "core/render/scene_scope.hpp"
#include "core/failure_state.hpp"
#include "core/render/modules/world/ray_tracing/submodules/world_prepare.hpp"

#include "core/render/buffers.hpp"
#include "core/render/chunks.hpp"
#include "core/render/entities.hpp"
#include "core/render/instancing.hpp"
#include "core/render/instancing_contract.hpp"
#include "core/render/modules/world/ray_tracing/ray_tracing_module.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/world.hpp"
#include "core/render/world_mesh_contract.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_set>
#include <glm/gtc/type_ptr.hpp>

WorldPrepare::WorldPrepare() {}

void WorldPrepare::init(std::shared_ptr<Framework> framework, std::shared_ptr<RayTracingModule> rayTracingModule) {
    framework_ = framework;
    rayTracingModule_ = rayTracingModule;
}

void WorldPrepare::build() {
    auto framework = framework_.lock();
    auto rayTracingModule = rayTracingModule_.lock();
    uint32_t size = framework->recordingContextCount();

    contexts_.resize(size);

    for (int i = 0; i < size; i++) {
        contexts_[i] = WorldPrepareContext::create(framework->contexts()[i], shared_from_this());
    }
}

void WorldPrepare::releaseScene() {
    {
        std::unique_lock<std::recursive_mutex> lock(entityRenderDataBatchesMtx_);
        previousEntityRenderDataBatches_.clear();
    }
    {
        std::unique_lock<std::recursive_mutex> lock(chunkTransformBatchesMtx_);
        previousChunkTransformBatches_.clear();
    }
    {
        std::unique_lock<std::recursive_mutex> lock(flywheelTransformMtx_);
        previousFlywheelTransforms_.clear();
    }
    previousRigidHistory_.clear();
    for (const auto &context : contexts_) {
        if (context == nullptr) continue;
        context->tlas = nullptr;
        context->tlasBuilder = nullptr;
        context->blasOffsetsBuffer = nullptr;
        context->indexBufferAddr = nullptr;
        context->positionBufferAddr = nullptr;
        context->materialBufferAddr = nullptr;
        context->lastIndexBufferAddr = nullptr;
        context->lastPositionBufferAddr = nullptr;
        context->lastObjToWorldMat = nullptr;
        context->instanceAppearanceBuffer = nullptr;
        context->hitGroups.clear();
    }
}

WorldPrepareContext::WorldPrepareContext(std::shared_ptr<FrameworkContext> frameworkContext,
                                         std::shared_ptr<WorldPrepare> worldPrepare)
    : frameworkContext(frameworkContext), worldPrepare(worldPrepare) {}

void WorldPrepareContext::uploadBuffer(std::vector<uint32_t> &blasOffsets,
                                       std::vector<uint64_t> &indexBufferAddrs,
                                       std::vector<uint64_t> &positionBufferAddrs,
                                       std::vector<uint64_t> &materialBufferAddrs,
                                       std::vector<uint64_t> &lastIndexBufferAddrs,
                                        std::vector<uint64_t> &lastPositionBufferAddrs,
                                        std::vector<glm::mat4> &lastObjToWorldMats,
                                        std::vector<vk::VertexFormat::InstanceAppearance> &instanceAppearances) {
    mcvr::profile::Phases profile("pt.metadata.allocate-copy");
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto vma = framework->vma();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();
    auto mainQueueIndex = physicalDevice->mainQueueIndex();
    auto cmdBuffer = context->worldCommandBuffer;

    blasOffsetsBuffer = vk::DeviceLocalBuffer::create(
        vma, device, blasOffsets.size() * sizeof(uint32_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    blasOffsetsBuffer->uploadToStagingBuffer(blasOffsets.data());

    indexBufferAddr = vk::DeviceLocalBuffer::create(
        vma, device, indexBufferAddrs.size() * sizeof(uint64_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    indexBufferAddr->uploadToStagingBuffer(indexBufferAddrs.data());

    positionBufferAddr = vk::DeviceLocalBuffer::create(
        vma, device, positionBufferAddrs.size() * sizeof(uint64_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    positionBufferAddr->uploadToStagingBuffer(positionBufferAddrs.data());

    materialBufferAddr = vk::DeviceLocalBuffer::create(
        vma, device, materialBufferAddrs.size() * sizeof(uint64_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    materialBufferAddr->uploadToStagingBuffer(materialBufferAddrs.data());

    lastIndexBufferAddr = vk::DeviceLocalBuffer::create(
        vma, device, lastIndexBufferAddrs.size() * sizeof(uint64_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    lastIndexBufferAddr->uploadToStagingBuffer(lastIndexBufferAddrs.data());

    lastPositionBufferAddr = vk::DeviceLocalBuffer::create(
        vma, device, lastPositionBufferAddrs.size() * sizeof(uint64_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    lastPositionBufferAddr->uploadToStagingBuffer(lastPositionBufferAddrs.data());

    lastObjToWorldMat = vk::DeviceLocalBuffer::create(
        vma, device, lastObjToWorldMats.size() * sizeof(glm::mat4),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    lastObjToWorldMat->uploadToStagingBuffer(lastObjToWorldMats.data());

    instanceAppearanceBuffer = vk::DeviceLocalBuffer::create(
        vma, device, instanceAppearances.size() * sizeof(vk::VertexFormat::InstanceAppearance),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    instanceAppearanceBuffer->uploadToStagingBuffer(instanceAppearances.data());

    profile.next("pt.metadata.upload-record");
    std::vector<std::shared_ptr<vk::DeviceLocalBuffer>> rayTracingMetaData{{
        blasOffsetsBuffer,
        indexBufferAddr,
        positionBufferAddr,
        materialBufferAddr,
        lastIndexBufferAddr,
        lastPositionBufferAddr,
        lastObjToWorldMat,
        instanceAppearanceBuffer,
    }};

    std::vector<vk::CommandBuffer::BufferMemoryBarrier> uploadPreBufferBarriers, uploadPostBufferBarriers;

    for (auto buffer : rayTracingMetaData) {
        if (buffer == nullptr) continue;
        uploadPreBufferBarriers.push_back({
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .buffer = buffer,
        });
        uploadPostBufferBarriers.push_back({
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .buffer = buffer,
        });
    }

    cmdBuffer->barriersBufferImage(uploadPreBufferBarriers, {});
    for (auto buffer : rayTracingMetaData) {
        if (buffer == nullptr) continue;
        buffer->uploadToBuffer(cmdBuffer);
    }
    cmdBuffer->barriersBufferImage(uploadPostBufferBarriers, {});
}

void WorldPrepareContext::render() {
    mcvr::profile::Scope profileOperation("pt.world-prepare");
    mcvr::profile::Phases profile("pt.prepare.setup");
    auto rayTracingContext = rayTracingModuleContext.lock();
    auto rayTracingModule = rayTracingContext != nullptr ? rayTracingContext->rayTracingModule.lock() : nullptr;
    auto worldPrepare1 = worldPrepare.lock();
    if (rayTracingModule == nullptr) { return; }
    if (worldPrepare1 == nullptr) { return; }

    std::shared_ptr<Framework> framework = Renderer::instance().framework();
    std::shared_ptr<FrameworkContext> context = frameworkContext.lock();
    std::shared_ptr<vk::VMA> vma = framework->vma();
    std::shared_ptr<vk::Device> device = framework->device();
    std::shared_ptr<vk::PhysicalDevice> physicalDevice = framework->physicalDevice();
    std::shared_ptr<vk::CommandBuffer> worldCommandBuffer = context->worldCommandBuffer;

    auto chunks = Renderer::instance().world()->chunks();
    auto entities = Renderer::instance().world()->entities();
    auto instancing = Renderer::instance().world()->instancing();
    auto cameraPos = Renderer::instance().world()->getCameraPos();

    auto chunkBuildScheduler = chunks->chunkBuildScheduler();
    profile.next("pt.prepare.chunk-schedule");
    if (chunkBuildScheduler != nullptr) {
        mcvr::failure::runCheckedStage([&] { chunkBuildScheduler->tryCheckBatchesFinish(); });
        mcvr::failure::runCheckedStage([&] {
            chunkBuildScheduler->tryScheduleBatches(chunkBuildScheduler->chunkBuildingBatchSize());
        });
    }

    profile.next("pt.prepare.chunk-lock");
    std::unique_lock<std::recursive_mutex> lock(chunks->mutex());
    profile.next("pt.prepare.blas-record");

    if (chunks->importantBLASBuilders().size() > 0) {
        vk::BLASBuilder::batchSubmit(chunks->importantBLASBuilders(), worldCommandBuffer);
    }

    entities->recordGpuConversion(worldCommandBuffer);
    const auto entityBlasStamp = SceneRecordingScope::active() ? -1 : context->auditGpu.begin(
        worldCommandBuffer->vkCommandBuffer(), "entity-blas-gpu");
    if (entities->blasBatchBuilder() != nullptr) { entities->blasBatchBuilder()->submit(worldCommandBuffer); }
    entities->recordCachedCloudBuild(worldCommandBuffer);
    entities->recordRigidModels(worldCommandBuffer);
    context->auditGpu.end(worldCommandBuffer->vkCommandBuffer(), entityBlasStamp);
    for (auto &builder : instancing->drainPendingBlasBuilders()) {
        if (builder != nullptr) builder->submit(worldCommandBuffer);
    }

    worldCommandBuffer->barriersMemory({vk::CommandBuffer::MemoryBarrier{
        .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .srcAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    }});

    profile.next("pt.prepare.metadata-init");
    uint32_t blasAccu = 0, blasGroupAccu = 0;
    std::vector<uint32_t> blasOffset;
    hitGroups.clear();
    const auto defaultGroup = mcvr::hitgroups::intern("default");
    std::vector<uint64_t> indexBufferAddrs;
    std::vector<uint64_t> positionBufferAddrs, materialBufferAddrs;
    std::vector<uint64_t> lastIndexBufferAddrs;
    std::vector<uint64_t> lastPositionBufferAddrs;
    std::vector<glm::mat4> lastObjToWorldMats;
    std::vector<vk::VertexFormat::InstanceAppearance> instanceAppearances;
    auto identityAppearance = [] {
        vk::VertexFormat::InstanceAppearance value{};
        value.colorMultiply = glm::vec4(1.0f);
        value.colorReplace = glm::vec4(1.0f);
        value.fluidProgress = 1.0f;
        return value;
    };

    // These counts are already published with the geometry. Allocate each CPU table once
    // instead of repeatedly relocating address/appearance arrays as the scene is appended.
    // Only capacity is predicted: the traversal below still validates and retains every owner.
    size_t instanceCapacity = 0, geometryCapacity = 0;
    const auto preparedEntityBatch = entities->entityBatch();
    const auto preparedInstances = instancing->preparedInstances();
    if (mcvr::profile::enabled()) {
        mcvr::profile::emit(mcvr::profile::frame, 5, "flywheel.instances", preparedInstances.size(), 0);
        mcvr::profile::emit(mcvr::profile::frame, 5, "flywheel.resident-models", instancing->residentModelCount(), 0);
    }
    const auto preparedRigid = entities->rigidInstanceBatch();
    if (preparedEntityBatch) {
        instanceCapacity += preparedEntityBatch->entities.size();
        for (const auto &entity : preparedEntityBatch->entities) geometryCapacity += entity->geometryCount;
    }
    if (preparedRigid) {
        // Every resident rigid model is exactly one geometry.
        instanceCapacity += preparedRigid->instances.size();
        geometryCapacity += preparedRigid->instances.size();
    }
    for (const auto &prepared : preparedInstances) {
        const auto &geometry = prepared.model->geometry;
        if (geometry && geometry->blas) {
            ++instanceCapacity;
            geometryCapacity += geometry->geometryCount;
        }
    }
    for (const auto &chunk : chunks->chunks()) {
        if (chunk->blas) {
            ++instanceCapacity;
            geometryCapacity += chunk->geometryCount;
        }
    }
    blasOffset.reserve(instanceCapacity);
    hitGroups.reserve(instanceCapacity + geometryCapacity);
    indexBufferAddrs.reserve(geometryCapacity);
    positionBufferAddrs.reserve(geometryCapacity);
    materialBufferAddrs.reserve(geometryCapacity);
    lastIndexBufferAddrs.reserve(geometryCapacity);
    lastPositionBufferAddrs.reserve(geometryCapacity);
    lastObjToWorldMats.reserve(instanceCapacity);
    instanceAppearances.reserve(geometryCapacity);

    auto scene = SceneRecordingScope::active();
    const uint32_t viewIndex = context->frameIndex / framework->swapchain()->imageCount();
    if (scene && scene->resetHistory) {
        worldPrepare1->previousEntityRenderDataBatches_.erase(viewIndex);
        worldPrepare1->previousRigidHistory_.erase(viewIndex);
        worldPrepare1->previousChunkTransformBatches_.erase(viewIndex);
        worldPrepare1->previousFlywheelTransforms_.erase(viewIndex);
    }
    tlasBuilder = vk::TLASBuilder::create();
    auto &instanceBuilder = tlasBuilder->beginInstanceBuilder();
    int blasIndex = 0;

    // Entity
    profile.next("pt.prepare.entity-metadata");
    {
        auto entityBatch = preparedEntityBatch;

        if (entityBatch != nullptr) {
            std::unique_lock<std::recursive_mutex> entityHistoryLock(worldPrepare1->entityRenderDataBatchesMtx_);
            auto &previousEntityRenderDataBatches = worldPrepare1->previousEntityRenderDataBatches_[(context->frameIndex / framework->swapchain()->imageCount())];
            auto &emptyEntityRenderDataBatch = worldPrepare1->emptyEntityRenderDataBatch_;

            auto &previousEntityRenderDataBatch = previousEntityRenderDataBatches.empty() ?
                                                      emptyEntityRenderDataBatch :
                                                      previousEntityRenderDataBatches.back();
            if (previousEntityRenderDataBatches.size() > Renderer::instance().framework()->swapchain()->imageCount())
                previousEntityRenderDataBatches.pop();
            auto &currentEntityRenderDataBatch = previousEntityRenderDataBatches.emplace();

            auto worldUniformBuffer = Renderer::instance().buffers()->worldUniformBuffer();
            auto ubo = static_cast<vk::Data::WorldUBO *>(worldUniformBuffer->mappedPtr());

            auto &entities1 = entityBatch->entities;
            for (int i = 0; i < entities1.size(); i++) {
                if (entities1[i]->worldToken != 0 &&
                    !entities->acceptsWorldMeshGeneration(entities1[i]->worldToken,
                        entities1[i]->frameToken, entities1[i]->resourceGeneration)) {
                    continue;
                }
                VkGeometryInstanceFlagsKHR flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
                if (entities1[i]->uiSceneOwner != 0) flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
                const mcvr::faces::ModelRules faceRules(entities1[i]->geometryMaterialFlags);
                // VkGeometryInstanceFlagsKHR flags = 0;
                VkTransformMatrixKHR transform;

                if (entities1[i]->prebuiltBLAS < 0) {
                    if (entities1[i]->coordinate == World::Coordinates::WORLD || !ubo) {
                        const auto translation = WorldMeshContract::worldTranslation(
                            entities1[i]->x, entities1[i]->y, entities1[i]->z,
                            cameraPos.x, cameraPos.y, cameraPos.z);
                        glm::mat4 instance = entities1[i]->instanceTransform;
                        instance[3] += glm::vec4(translation[0], translation[1], translation[2], 0);
                        transform = {
                            instance[0][0], instance[1][0], instance[2][0], instance[3][0],
                            instance[0][1], instance[1][1], instance[2][1], instance[3][1],
                            instance[0][2], instance[1][2], instance[2][2], instance[3][2],
                        };
                    } else if (entities1[i]->coordinate == World::Coordinates::CAMERA) {
                        glm::mat4 viewMat = glm::transpose(ubo->cameraViewMatInv); // column major to row major

                        transform = {
                            viewMat[0][0], viewMat[0][1], viewMat[0][2], viewMat[0][3], //
                            viewMat[1][0], viewMat[1][1], viewMat[1][2], viewMat[1][3], //
                            viewMat[2][0], viewMat[2][1], viewMat[2][2], viewMat[2][3], //
                        };
                    } else if (entities1[i]->coordinate == World::Coordinates::CAMERA_SHIFT) {
                        glm::vec3 shift = glm::vec3(ubo->cameraViewMatInv[3]);
                        transform = {
                            1, 0, 0, shift.x, //
                            0, 1, 0, shift.y, //
                            0, 0, 1, shift.z, //
                        };
                    }

                    flags = faceRules.instanceFlags(transform) |
                        (entities1[i]->uiSceneOwner ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : 0);
                    instanceBuilder.defineInstance(transform, blasIndex, entities1[i]->rayTracingFlag, blasGroupAccu,
                                                   flags, entities1[i]->blas);
                } else {
                    // auto &prebuiltBLAS =
                    //     Renderer::instance().framework()->prebuiltBLASs()[entityRenderData->prebuiltBLAS];
                    // transform = prebuiltBLAS.align(*entityRenderData->vertices, *entityRenderData->indices);

                    // instanceBuilder.defineInstance(transform, blasIndex, entityRenderData->rayTracingFlag,
                    // blasGroupAccu, flags,
                    //                                prebuiltBLAS.blas);
                    throw std::runtime_error("prebuilt blas not implemented yet!");
                }

                hitGroups.push_back(mcvr::hitgroups::shadow);
                for (int j = 0; j < entities1[i]->geometryCount; j++) {
                    if (entities1[i]->geometryGroupNames != nullptr &&
                        j < static_cast<int>(entities1[i]->geometryGroupNames->size())) {
                        hitGroups.push_back(mcvr::hitgroups::intern((*entities1[i]->geometryGroupNames)[j]));
                    } else {
                        hitGroups.push_back(defaultGroup);
                    }
                }

                for (int j = 0; j < entities1[i]->geometryCount; j++) {
                    indexBufferAddrs.push_back((*entities1[i]->indexBufferAddresses)[j]);
                    positionBufferAddrs.push_back((*entities1[i]->positionBufferAddresses)[j]);
                    materialBufferAddrs.push_back((*entities1[i]->materialBufferAddresses)[j]);
                    auto appearance = identityAppearance();
                    appearance.materialFlags = faceRules.shaderFlags(entities1[i]->geometryMaterialFlags[j]) | (entities1[i]->uiSceneOwner << 24u);
                    instanceAppearances.push_back(appearance);
                }

                {
                    if (entities1[i]->historyKey()) {
                        currentEntityRenderDataBatch[entities1[i]->historyKey()].first = entities1[i];
                        currentEntityRenderDataBatch[entities1[i]->historyKey()].second = transform;
                    }
                }

                {
                    glm::mat4 lastObjToWorldMat(1);
                    auto iter = previousEntityRenderDataBatch.find(entities1[i]->historyKey());
                    if (iter != previousEntityRenderDataBatch.end()) {
                        auto &previousEntityRenderData = (*iter).second.first;
                        if (previousEntityRenderData->geometryCount == entities1[i]->geometryCount) {
                            for (int j = 0; j < entities1[i]->geometryCount; j++) {
                                if ((*previousEntityRenderData->vertexCounts)[j] == (*entities1[i]->vertexCounts)[j] &&
                                    (*previousEntityRenderData->indexCounts)[j] == (*entities1[i]->indexCounts)[j]) {
                                    lastIndexBufferAddrs.push_back(
                                        (*previousEntityRenderData->indexBufferAddresses)[j]);
                                    lastPositionBufferAddrs.push_back(
                                        (*previousEntityRenderData->positionBufferAddresses)[j]);
                                } else {
                                    lastIndexBufferAddrs.push_back(0);
                                    lastPositionBufferAddrs.push_back(0);
                                }
                            }
                        } else {
                            for (int j = 0; j < entities1[i]->geometryCount; j++) {
                                lastIndexBufferAddrs.push_back(0);
                                lastPositionBufferAddrs.push_back(0);
                            }
                        }

                        VkTransformMatrixKHR lastObjToWorldVkMat = iter->second.second;
                        lastObjToWorldMat = glm::transpose(glm::mat4(glm::make_vec4(lastObjToWorldVkMat.matrix[0]), //
                                                                     glm::make_vec4(lastObjToWorldVkMat.matrix[1]), //
                                                                     glm::make_vec4(lastObjToWorldVkMat.matrix[2]), //
                                                                     glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)));
                    } else {
                        for (int j = 0; j < entities1[i]->geometryCount; j++) {
                            lastIndexBufferAddrs.push_back(0);
                            lastPositionBufferAddrs.push_back(0);
                        }
                    }
                    lastObjToWorldMats.push_back(lastObjToWorldMat);
                }

                blasOffset.push_back(blasAccu);
                blasAccu += entities1[i]->geometryCount;
                blasGroupAccu += entities1[i]->geometryCount + 1;

                blasIndex++;
            }
        }
    }

    // Rigid persistent-model instances. Geometry, BLAS, hit group and face rules are resolved once
    // per resident model; each instance contributes its transform, mask and motion history.
    profile.next("pt.prepare.rigid-metadata");
    {
        std::unique_lock<std::recursive_mutex> rigidHistoryLock(worldPrepare1->entityRenderDataBatchesMtx_);
        auto &views = worldPrepare1->previousRigidHistory_[viewIndex];
        const auto previous = views.empty() ? nullptr : views.back();
        // Record every frame, even an empty one, so history always means the immediately
        // preceding frame of this view, as in the per-entity map.
        auto current = std::make_shared<WorldPrepare::RigidHistoryState>();
        const bool accepted = preparedRigid && !preparedRigid->instances.empty() &&
            entities->acceptsWorldMeshGeneration(preparedRigid->worldToken, preparedRigid->frameToken,
                                                 preparedRigid->resourceGeneration);
        if (accepted) {
            framework->frameResourceRetainer().retain(preparedRigid);
            current->owner = preparedRigid;
            current->frame.reserve(preparedRigid->instances.size());
            for (const auto &instance : preparedRigid->instances) {
                const auto &model = *instance.model;
                const auto translation = WorldMeshContract::worldTranslation(
                    instance.x, instance.y, instance.z, cameraPos.x, cameraPos.y, cameraPos.z);
                glm::mat4 placed = instance.transform;
                placed[3] += glm::vec4(translation[0], translation[1], translation[2], 0);
                const VkTransformMatrixKHR transform = {
                    placed[0][0], placed[1][0], placed[2][0], placed[3][0],
                    placed[0][1], placed[1][1], placed[2][1], placed[3][1],
                    placed[0][2], placed[1][2], placed[2][2], placed[3][2],
                };
                instanceBuilder.defineInstance(transform, blasIndex, instance.mask, blasGroupAccu,
                                               model.faces.instanceFlags(transform), model.blas);
                hitGroups.push_back(mcvr::hitgroups::shadow);
                hitGroups.push_back(model.groupId);
                indexBufferAddrs.push_back(model.indexAddress);
                positionBufferAddrs.push_back(model.positionAddress);
                materialBufferAddrs.push_back(model.materialAddress);
                auto appearance = identityAppearance();
                appearance.materialFlags = model.faces.shaderFlags(model.materialFlags);
                instanceAppearances.push_back(appearance);

                const auto last = mcvr::rigid::previous(previous ? previous->frame.find(instance.history) : nullptr,
                                                        instance.key, model.vertexCount, model.indexCount);
                lastIndexBufferAddrs.push_back(last.indexAddress);
                lastPositionBufferAddrs.push_back(last.positionAddress);
                lastObjToWorldMats.push_back(glm::transpose(glm::mat4(
                    glm::make_vec4(last.transform.data()), glm::make_vec4(last.transform.data() + 4),
                    glm::make_vec4(last.transform.data() + 8), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f))));

                mcvr::rigid::HistoryEntry entry{.history = instance.history, .model = instance.key,
                    .indexAddress = model.indexAddress, .positionAddress = model.positionAddress,
                    .vertexCount = model.vertexCount, .indexCount = model.indexCount};
                std::memcpy(entry.transform.data(), &transform, sizeof(transform));
                current->frame.add(entry);

                blasOffset.push_back(blasAccu);
                blasAccu += 1;
                blasGroupAccu += 2;
                ++blasIndex;
            }
        }
        current->frame.finish();
        // Same retention depth as the per-entity history: pop before push, up to imageCount + 1.
        if (views.size() > framework->swapchain()->imageCount()) views.pop();
        views.push(std::move(current));
    }

    // Flywheel shared-model instances. Model vertex/material/index buffers and BLAS are reused;
    // only the TLAS transform and the small appearance record vary per instance.
    profile.next("pt.prepare.flywheel-lock");
    std::unique_lock<std::recursive_mutex> flywheelHistoryLock(worldPrepare1->flywheelTransformMtx_);
    profile.next("pt.prepare.flywheel-metadata");
    auto previousFlywheelTransforms = std::move(worldPrepare1->previousFlywheelTransforms_[(context->frameIndex / framework->swapchain()->imageCount())]);
    std::map<WorldPrepare::FlywheelInstanceKey, glm::mat4> currentFlywheelTransforms;
    for (const auto &prepared : preparedInstances) {
        auto geometry = prepared.model->geometry;
        if (geometry == nullptr || geometry->blas == nullptr) continue;
        const mcvr::faces::ModelRules faceRules(prepared.model->materialFlags);
        framework->frameResourceRetainer().retain(prepared.model);
        glm::mat4 objectToWorld = mcvr::instancing::cameraRelativeTransform(
            prepared.renderOrigin, prepared.transform, cameraPos);
        VkTransformMatrixKHR transform = {
            objectToWorld[0][0], objectToWorld[1][0], objectToWorld[2][0], objectToWorld[3][0],
            objectToWorld[0][1], objectToWorld[1][1], objectToWorld[2][1], objectToWorld[3][1],
            objectToWorld[0][2], objectToWorld[1][2], objectToWorld[2][2], objectToWorld[3][2],
        };
        instanceBuilder.defineInstance(transform, blasIndex, 0x01, blasGroupAccu,
            faceRules.instanceFlags(transform), geometry->blas);
        hitGroups.push_back(mcvr::hitgroups::shadow);
        for (int j = 0; j < geometry->geometryCount; ++j) {
            hitGroups.push_back(prepared.model->groupIds[j]);
            indexBufferAddrs.push_back((*geometry->indexBufferAddresses)[j]);
            positionBufferAddrs.push_back((*geometry->positionBufferAddresses)[j]);
            materialBufferAddrs.push_back((*geometry->materialBufferAddresses)[j]);
            lastIndexBufferAddrs.push_back((*geometry->indexBufferAddresses)[j]);
            lastPositionBufferAddrs.push_back((*geometry->positionBufferAddresses)[j]);
            auto appearance = prepared.appearance;
            appearance.materialFlags = faceRules.shaderFlags(prepared.model->materialFlags[j]);
            if ((appearance.flags & (1u << 12u)) == 0u) {
                appearance.lightTransform = objectToWorld;
                appearance.lightTransform[3][0] += static_cast<float>(cameraPos.x);
                appearance.lightTransform[3][1] += static_cast<float>(cameraPos.y);
                appearance.lightTransform[3][2] += static_cast<float>(cameraPos.z);
            }
            instanceAppearances.push_back(appearance);
        }
        WorldPrepare::FlywheelInstanceKey historyKey{prepared.engine, prepared.id};
        auto previous = previousFlywheelTransforms.find(historyKey);
        lastObjToWorldMats.push_back(previous == previousFlywheelTransforms.end()
            ? objectToWorld : previous->second);
        currentFlywheelTransforms.emplace(historyKey, objectToWorld);
        blasOffset.push_back(blasAccu);
        blasAccu += geometry->geometryCount;
        blasGroupAccu += geometry->geometryCount + 1;
        ++blasIndex;
    }
    worldPrepare1->previousFlywheelTransforms_[(context->frameIndex / framework->swapchain()->imageCount())] = std::move(currentFlywheelTransforms);

    profile.next("pt.prepare.chunk-metadata");
    // Chunk
    {
        profile.next("pt.prepare.chunk-history-lock");
        std::unique_lock<std::recursive_mutex> chunkHistoryLock(worldPrepare1->chunkTransformBatchesMtx_);
        profile.next("pt.prepare.chunk-metadata");
        auto &previousChunkTransformBatches = worldPrepare1->previousChunkTransformBatches_[(context->frameIndex / framework->swapchain()->imageCount())];
        auto &emptyChunkTransformBatch = worldPrepare1->emptyChunkTransformBatch_;
        auto &previousChunkTransformBatch = previousChunkTransformBatches.empty() ?
                                                emptyChunkTransformBatch :
                                                previousChunkTransformBatches.back();
        if (previousChunkTransformBatches.size() > Renderer::instance().framework()->swapchain()->imageCount()) {
            previousChunkTransformBatches.pop();
        }
        auto &currentChunkTransformBatch = previousChunkTransformBatches.emplace();

        static const bool cacheEnabled = [] {
            const char *value = std::getenv("MCVR_CHUNK_SCENE_CACHE");
            return !value || std::string_view(value) != "0";
        }();
        static const bool verifyCache = [] {
            const char *value = std::getenv("MCVR_CHUNK_SCENE_VERIFY");
            return value && std::string_view(value) == "1";
        }();
        uint64_t cached = 0, rebuilt = 0, customHistory = 0, verified = 0;
        auto &chunk1s = chunks->chunks();
        for (int i = 0; i < chunk1s.size(); i++) {
            auto &chunk1 = chunk1s[i];
            if (mcvr::chunkTrace::enabled) {
                auto version=chunk1->blas ? chunk1->blasVersion : chunk1->desiredVersion;
                if (version!=chunk1->lastTracedVersion || bool(chunk1->blas)!=chunk1->lastTracedPresent) {
                    const bool observablePresence = bool(chunk1->blas) || chunk1->lastTracedPresent;
                    chunk1->lastTracedVersion=version;
                    chunk1->lastTracedPresent=bool(chunk1->blas);
                    if (observablePresence) {
                        auto frame=framework->safeAcquireCurrentContext();
                        mcvr::chunkTrace::note(chunk1->blas?"tlas-record":"tlas-absent",i,version,
                            frame ? frame->chunkTraceSerial : 0);
                    }
                }
            }
            if (chunk1->blas == nullptr) continue;
            std::shared_ptr<mcvr::ChunkSceneMetadata> metadata;
            if (cacheEnabled) {
                auto builds = chunk1->sceneMetadataCache.builds();
                metadata = chunk1->sceneMetadata();
                rebuilt += builds != chunk1->sceneMetadataCache.builds();
                cached += builds == chunk1->sceneMetadataCache.builds();
                // Retain the immutable published snapshot, never the mutable Chunk1 owner.
                framework->frameResourceRetainer().retain(metadata);
            } else {
                chunk1->retainResources(framework->frameResourceRetainer());
            }
            const auto faceRules = metadata ? metadata->faces : mcvr::faces::ModelRules(*chunk1->geometryMaterialFlags);

            glm::dmat4 objectToWorld = chunk1->customTransform;
            if (!chunk1->hasCustomTransform) {
                objectToWorld = glm::dmat4(1.0);
                objectToWorld[3] = glm::dvec4(chunk1->x, chunk1->y, chunk1->z, 1.0);
            }
            objectToWorld[3][0] -= cameraPos.x;
            objectToWorld[3][1] -= cameraPos.y;
            objectToWorld[3][2] -= cameraPos.z;
            // External slots may receive their first transform after geometry publication.
            // Preserve that preceding frame too; only primary slots cannot become custom.
            mcvr::recordChunkTransform(currentChunkTransformBatch, chunk1, objectToWorld,
                chunk1->hasCustomTransform || uint32_t(i) >= chunks->primaryChunkCount(), !cacheEnabled);
            customHistory += chunk1->hasCustomTransform;

            VkTransformMatrixKHR transform = {
                static_cast<float>(objectToWorld[0][0]), static_cast<float>(objectToWorld[1][0]),
                static_cast<float>(objectToWorld[2][0]), static_cast<float>(objectToWorld[3][0]),
                static_cast<float>(objectToWorld[0][1]), static_cast<float>(objectToWorld[1][1]),
                static_cast<float>(objectToWorld[2][1]), static_cast<float>(objectToWorld[3][1]),
                static_cast<float>(objectToWorld[0][2]), static_cast<float>(objectToWorld[1][2]),
                static_cast<float>(objectToWorld[2][2]), static_cast<float>(objectToWorld[3][2]),
            };

            instanceBuilder.defineInstance(transform, blasIndex, 0x01, blasGroupAccu,
                faceRules.instanceFlags(transform), chunk1->blas);

            const size_t firstGroup = hitGroups.size(), firstGeometry = indexBufferAddrs.size();
            if (metadata) {
                metadata->append(chunk1->hasCustomTransform, hitGroups, indexBufferAddrs,
                    positionBufferAddrs, materialBufferAddrs, lastIndexBufferAddrs,
                    lastPositionBufferAddrs, instanceAppearances);
            } else {
                hitGroups.push_back(mcvr::hitgroups::shadow);
                for (int j = 0; j < chunk1->geometryCount; j++) {
                    if (chunk1->geometryGroupNames != nullptr && j < static_cast<int>(chunk1->geometryGroupNames->size())) {
                        hitGroups.push_back(mcvr::hitgroups::intern((*chunk1->geometryGroupNames)[j]));
                    } else {
                        hitGroups.push_back(defaultGroup);
                    }
                }

                for (int j = 0; j < chunk1->geometryCount; j++) {
                    indexBufferAddrs.push_back((*chunk1->indexBufferAddresses)[j]);
                    positionBufferAddrs.push_back((*chunk1->positionBufferAddresses)[j]);
                    materialBufferAddrs.push_back((*chunk1->materialBufferAddresses)[j]);
                    auto appearance = identityAppearance();
                    if (chunk1->geometryMaterialFlags != nullptr &&
                        j < static_cast<int>(chunk1->geometryMaterialFlags->size())) {
                        appearance.materialFlags = faceRules.shaderFlags((*chunk1->geometryMaterialFlags)[j]);
                    }
                    instanceAppearances.push_back(appearance);
                    if (chunk1->hasCustomTransform) {
                        lastIndexBufferAddrs.push_back((*chunk1->indexBufferAddresses)[j]);
                        lastPositionBufferAddrs.push_back((*chunk1->positionBufferAddresses)[j]);
                    } else {
                        lastIndexBufferAddrs.push_back(0);
                        lastPositionBufferAddrs.push_back(0);
                    }
                }
            }
            if (metadata && verifyCache) {
                // Independent reference from the current published source, after append.
                const mcvr::faces::ModelRules referenceFaces(*chunk1->geometryMaterialFlags);
                if (faceRules.instanceFlags(transform) != referenceFaces.instanceFlags(transform) ||
                    hitGroups[firstGroup] != mcvr::hitgroups::shadow)
                    throw std::logic_error("Chunk scene cache TLAS/group mismatch");
                for (uint32_t j = 0; j < chunk1->geometryCount; ++j) {
                    const auto k = firstGeometry + j;
                    auto expected = identityAppearance();
                    expected.materialFlags = referenceFaces.shaderFlags((*chunk1->geometryMaterialFlags)[j]);
                    const auto group = chunk1->geometryGroupNames && j < chunk1->geometryGroupNames->size()
                        ? (*chunk1->geometryGroupNames)[j] : "default";
                    if (hitGroups[firstGroup + 1 + j] != mcvr::hitgroups::intern(group) ||
                        indexBufferAddrs[k] != (*chunk1->indexBufferAddresses)[j] ||
                        positionBufferAddrs[k] != (*chunk1->positionBufferAddresses)[j] ||
                        materialBufferAddrs[k] != (*chunk1->materialBufferAddresses)[j] ||
                        lastIndexBufferAddrs[k] != (chunk1->hasCustomTransform ? (*chunk1->indexBufferAddresses)[j] : 0) ||
                        lastPositionBufferAddrs[k] != (chunk1->hasCustomTransform ? (*chunk1->positionBufferAddresses)[j] : 0) ||
                        std::memcmp(&expected, &instanceAppearances[k], sizeof(expected)))
                        throw std::logic_error("Chunk scene cache geometry mismatch");
                }
                ++verified;
            }

            {
                const auto previousObjectToWorld = mcvr::previousChunkTransform(
                    previousChunkTransformBatch, chunk1, objectToWorld, chunk1->hasCustomTransform);
                if (verifyCache && !chunk1->hasCustomTransform && previousObjectToWorld != objectToWorld)
                    throw std::logic_error("Static chunk history must use its current camera-relative transform");
                glm::mat4 lastObjToWorldMat = glm::mat4(previousObjectToWorld);
                lastObjToWorldMats.push_back(lastObjToWorldMat);
            }

            blasOffset.push_back(blasAccu);
            blasAccu += chunk1->geometryCount;
            blasGroupAccu += chunk1->geometryCount + 1;

            blasIndex++;
        }
        if (mcvr::profile::enabled()) {
            mcvr::profile::emit(mcvr::profile::frame, 5, "chunk.scene.cache-hit", cached, 0);
            mcvr::profile::emit(mcvr::profile::frame, 5, "chunk.scene.cache-build", rebuilt, 0);
            mcvr::profile::emit(mcvr::profile::frame, 5, "chunk.scene.custom-history", customHistory, 0);
        }
        // Residency census: device bytes owned by published slots, each buffer counted once even
        // when a build batch shares it, against builds still queued or in flight. Profile counters
        // every 30 profiled frames; RADIANCE_CHUNK_CENSUS=1 also logs it about every 60 frames.
        static const bool censusLog = [] {
            const char *value = std::getenv("RADIANCE_CHUNK_CENSUS");
            return value && std::string_view(value) == "1";
        }();
        static uint64_t censusFrames = 0;
        const bool censusProfile = mcvr::profile::enabled() && mcvr::profile::frame % 30 == 0;
        const bool censusLogNow = censusLog && ++censusFrames % 60 == 0;
        if (censusProfile || censusLogNow) {
            std::unordered_set<const void *> counted;
            uint64_t slots = 0, slotBytes = 0;
            auto add = [&](const auto &buffer) {
                if (buffer && counted.insert(buffer.get()).second) slotBytes += buffer->size();
            };
            for (const auto &chunk1 : chunk1s) {
                if (!chunk1->blas) continue;
                ++slots;
                add(chunk1->indexBuffer);
                add(chunk1->positionBuffer);
                add(chunk1->materialBuffer);
                add(chunk1->lightBuffer);
                add(chunk1->blas->blasBuffer());
            }
            const auto census = chunkBuildScheduler != nullptr ? chunkBuildScheduler->census() : ChunkBuildScheduler::Census{};
            if (censusProfile) {
                mcvr::profile::emit(mcvr::profile::frame, 5, "chunk.resident.slots", slots, slotBytes);
                mcvr::profile::emit(mcvr::profile::frame, 5, "chunk.resident.queued", census.queued, 0);
                mcvr::profile::emit(mcvr::profile::frame, 5, "chunk.resident.inflight", census.chunks, census.bytes);
            }
            if (censusLogNow)
                mcvr::log::info("ChunkResidency") << "slots=" << slots << " slotBytes=" << slotBytes
                    << " queued=" << census.queued << " inflightBatches=" << census.batches
                    << " inflightChunks=" << census.chunks << " inflightBytes=" << census.bytes
                    << chunks->orphanCensus() << framework->frameResourceRetainer().census() << std::endl;
        }
        if (verifyCache) {
            static std::atomic<uint64_t> verifiedFrames{0}, verifiedChunks{0};
            const auto chunkTotal = verifiedChunks.fetch_add(verified, std::memory_order_relaxed) + verified;
            const auto frameTotal = verifiedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
            if (frameTotal == 1 || frameTotal % 300 == 0)
                mcvr::log::info("ChunkSceneCheck") << "cache=" << cacheEnabled << " frames=" << frameTotal
                    << " checkedChunks=" << chunkTotal << " cached=" << cached << " rebuilt=" << rebuilt
                    << " customHistory=" << customHistory << std::endl;
        }
    }

    profile.next("pt.prepare.tlas-build-record");
    if (mcvr::profile::enabled()) mcvr::profile::emit(mcvr::profile::frame,5,
        "world.tlas.instances",instanceBuilder.instances.size(),0);
    if (instanceBuilder.instances.empty()) {
        tlas = nullptr;
        return;
    }

    if (scene && scene->batchTlas) {
        tlas = scene->batchTlas;
    } else {
    const auto tlasStamp = SceneRecordingScope::active() ? -1 : context->auditGpu.begin(
        worldCommandBuffer->vkCommandBuffer(), "tlas-build-gpu");
    tlas = instanceBuilder.endInstanceBuilder(device, vma)
               ->defineBuildProperty(VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR)
               ->querySizeInfo(device)
               ->allocateBuffers(physicalDevice, device, vma)
               ->buildAndSubmit(device, worldCommandBuffer);
    context->auditGpu.end(worldCommandBuffer->vkCommandBuffer(), tlasStamp);

        if (scene) { scene->batchTlas = tlas; ++scene->batchTlasBuilds; }
    }

    worldCommandBuffer->barriersMemory({vk::CommandBuffer::MemoryBarrier{
        .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    }});

    profile.next("pt.prepare.metadata-upload");
    uploadBuffer(blasOffset, indexBufferAddrs, positionBufferAddrs, materialBufferAddrs,
                 lastIndexBufferAddrs, lastPositionBufferAddrs, lastObjToWorldMats,
                 instanceAppearances);
    profile.next("pt.prepare.cleanup");
}

void WorldPrepareContext::setupHitGroupSbt(const std::unordered_map<std::string, uint32_t> &hitGroupNameToIndex,
                                           uint32_t fallbackHitGroupIndex,
                                           uint32_t shadowHitGroupIndex,
                                           std::shared_ptr<vk::CommandBuffer> commandBuffer,
                                           std::shared_ptr<vk::SBT> updateSbt,
                                           std::shared_ptr<vk::SBT> querySbt) {
    mcvr::profile::Phases profile("pt.sbt.lookup");
    // One name lookup per registered group, then an array index per geometry.
    const auto table = mcvr::hitgroups::Registry::global().resolve(hitGroupNameToIndex, fallbackHitGroupIndex,
                                                                   shadowHitGroupIndex);
    std::vector<uint32_t> hitGroupIndices;
    hitGroupIndices.reserve(hitGroups.size());
    for (const auto group : hitGroups) {
        if (group >= table.size()) throw std::logic_error("Hit-group id was interned after SBT resolution");
        hitGroupIndices.push_back(table[group]);
    }

    profile.next("pt.sbt.allocate-copy-record");
    if (updateSbt != nullptr) { updateSbt->setupHitSBT(hitGroupIndices, commandBuffer); }
    if (querySbt != nullptr) { querySbt->setupHitSBT(hitGroupIndices, commandBuffer); }
}
