#include "core/render/chunk_trace.hpp"
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
#include <filesystem>
#include <glm/gtc/type_ptr.hpp>

WorldPrepare::WorldPrepare() {}

namespace {
uint32_t shaderMaterialFlags(uint32_t material, std::span<const uint32_t> model) {
    return mcvr::faces::uniform(model) ? (material & ~mcvr::faces::mask) : material;
}
}

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
    if (chunkBuildScheduler != nullptr) {
        mcvr::failure::runCheckedStage([&] { chunkBuildScheduler->tryCheckBatchesFinish(); });
        mcvr::failure::runCheckedStage([&] {
            chunkBuildScheduler->tryScheduleBatches(chunkBuildScheduler->chunkBuildingBatchSize());
        });
    }

    std::unique_lock<std::recursive_mutex> lock(chunks->mutex());

    if (chunks->importantBLASBuilders().size() > 0) {
        vk::BLASBuilder::batchSubmit(chunks->importantBLASBuilders(), worldCommandBuffer);
    }

    if (entities->blasBatchBuilder() != nullptr) { entities->blasBatchBuilder()->submit(worldCommandBuffer); }
    entities->recordCachedCloudBuild(worldCommandBuffer);
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

    uint32_t blasAccu = 0, blasGroupAccu = 0;
    std::vector<uint32_t> blasOffset;
    hitGroupNames.clear();
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

    auto scene = SceneRecordingScope::active();
    const uint32_t viewIndex = context->frameIndex / framework->swapchain()->imageCount();
    if (scene && scene->resetHistory) {
        worldPrepare1->previousEntityRenderDataBatches_.erase(viewIndex);
        worldPrepare1->previousChunkTransformBatches_.erase(viewIndex);
        worldPrepare1->previousFlywheelTransforms_.erase(viewIndex);
    }
    tlasBuilder = vk::TLASBuilder::create();
    auto &instanceBuilder = tlasBuilder->beginInstanceBuilder();
    int blasIndex = 0;

    // Entity
    {
        auto entityBatch = entities->entityBatch();

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

                    flags = mcvr::faces::instanceFlags(entities1[i]->geometryMaterialFlags, transform) |
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

                hitGroupNames.push_back("shadow");
                for (int j = 0; j < entities1[i]->geometryCount; j++) {
                    const std::string &groupName =
                        entities1[i]->geometryGroupNames != nullptr &&
                                j < static_cast<int>(entities1[i]->geometryGroupNames->size()) ?
                            (*entities1[i]->geometryGroupNames)[j] :
                            "default";
                    hitGroupNames.push_back(groupName);
                }

                for (int j = 0; j < entities1[i]->geometryCount; j++) {
                    indexBufferAddrs.push_back((*entities1[i]->indexBufferAddresses)[j]);
                    positionBufferAddrs.push_back((*entities1[i]->positionBufferAddresses)[j]);
                    materialBufferAddrs.push_back((*entities1[i]->materialBufferAddresses)[j]);
                    auto appearance = identityAppearance();
                    appearance.materialFlags = shaderMaterialFlags(entities1[i]->geometryMaterialFlags[j], entities1[i]->geometryMaterialFlags) | (entities1[i]->uiSceneOwner << 24u);
                    instanceAppearances.push_back(appearance);
                }

                {
                    if (entities1[i]->hashCode) {
                        currentEntityRenderDataBatch[entities1[i]->hashCode].first = entities1[i];
                        currentEntityRenderDataBatch[entities1[i]->hashCode].second = transform;
                    }
                }

                {
                    glm::mat4 lastObjToWorldMat(1);
                    auto iter = previousEntityRenderDataBatch.find(entities1[i]->hashCode);
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

    // Flywheel shared-model instances. Model vertex/material/index buffers and BLAS are reused;
    // only the TLAS transform and the small appearance record vary per instance.
    std::unique_lock<std::recursive_mutex> flywheelHistoryLock(worldPrepare1->flywheelTransformMtx_);
    auto previousFlywheelTransforms = std::move(worldPrepare1->previousFlywheelTransforms_[(context->frameIndex / framework->swapchain()->imageCount())]);
    std::map<WorldPrepare::FlywheelInstanceKey, glm::mat4> currentFlywheelTransforms;
    for (const auto &prepared : instancing->preparedInstances()) {
        auto geometry = prepared.model->geometry;
        if (geometry == nullptr || geometry->blas == nullptr) continue;
        framework->frameResourceRetainer().retain(prepared.model);
        glm::mat4 objectToWorld = mcvr::instancing::cameraRelativeTransform(
            prepared.renderOrigin, prepared.transform, cameraPos);
        VkTransformMatrixKHR transform = {
            objectToWorld[0][0], objectToWorld[1][0], objectToWorld[2][0], objectToWorld[3][0],
            objectToWorld[0][1], objectToWorld[1][1], objectToWorld[2][1], objectToWorld[3][1],
            objectToWorld[0][2], objectToWorld[1][2], objectToWorld[2][2], objectToWorld[3][2],
        };
        instanceBuilder.defineInstance(transform, blasIndex, 0x01, blasGroupAccu,
            mcvr::faces::instanceFlags(prepared.model->materialFlags, transform), geometry->blas);
        hitGroupNames.push_back("shadow");
        for (int j = 0; j < geometry->geometryCount; ++j) {
            hitGroupNames.push_back((*geometry->geometryGroupNames)[j]);
            indexBufferAddrs.push_back((*geometry->indexBufferAddresses)[j]);
            positionBufferAddrs.push_back((*geometry->positionBufferAddresses)[j]);
            materialBufferAddrs.push_back((*geometry->materialBufferAddresses)[j]);
            lastIndexBufferAddrs.push_back((*geometry->indexBufferAddresses)[j]);
            lastPositionBufferAddrs.push_back((*geometry->positionBufferAddresses)[j]);
            auto appearance = prepared.appearance;
            appearance.materialFlags = shaderMaterialFlags(prepared.model->materialFlags[j], prepared.model->materialFlags);
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

    // Chunk
    {
        std::unique_lock<std::recursive_mutex> chunkHistoryLock(worldPrepare1->chunkTransformBatchesMtx_);
        auto &previousChunkTransformBatches = worldPrepare1->previousChunkTransformBatches_[(context->frameIndex / framework->swapchain()->imageCount())];
        auto &emptyChunkTransformBatch = worldPrepare1->emptyChunkTransformBatch_;
        auto &previousChunkTransformBatch = previousChunkTransformBatches.empty() ?
                                                emptyChunkTransformBatch :
                                                previousChunkTransformBatches.back();
        if (previousChunkTransformBatches.size() > Renderer::instance().framework()->swapchain()->imageCount()) {
            previousChunkTransformBatches.pop();
        }
        auto &currentChunkTransformBatch = previousChunkTransformBatches.emplace();

        auto &chunk1s = chunks->chunks();
        for (int i = 0; i < chunk1s.size(); i++) {
            auto &chunk1 = chunk1s[i];
            if (mcvr::chunkTrace::enabled) {
                auto version=chunk1->blas ? chunk1->blasVersion : chunk1->desiredVersion;
                if (version!=chunk1->lastTracedVersion || bool(chunk1->blas)!=chunk1->lastTracedPresent) {
                    chunk1->lastTracedVersion=version;
                    chunk1->lastTracedPresent=bool(chunk1->blas);
                    auto frame=framework->safeAcquireCurrentContext();
                    mcvr::chunkTrace::note(chunk1->blas?"tlas-record":"tlas-absent",i,version,
                        frame ? frame->chunkTraceSerial : 0);
                }
            }
            if (chunk1->blas == nullptr) continue;
            chunk1->retainResources(framework->frameResourceRetainer());

            glm::dmat4 objectToWorld = chunk1->customTransform;
            if (!chunk1->hasCustomTransform) {
                objectToWorld = glm::dmat4(1.0);
                objectToWorld[3] = glm::dvec4(chunk1->x, chunk1->y, chunk1->z, 1.0);
            }
            objectToWorld[3][0] -= cameraPos.x;
            objectToWorld[3][1] -= cameraPos.y;
            objectToWorld[3][2] -= cameraPos.z;
            currentChunkTransformBatch[chunk1] = objectToWorld;

            VkTransformMatrixKHR transform = {
                static_cast<float>(objectToWorld[0][0]), static_cast<float>(objectToWorld[1][0]),
                static_cast<float>(objectToWorld[2][0]), static_cast<float>(objectToWorld[3][0]),
                static_cast<float>(objectToWorld[0][1]), static_cast<float>(objectToWorld[1][1]),
                static_cast<float>(objectToWorld[2][1]), static_cast<float>(objectToWorld[3][1]),
                static_cast<float>(objectToWorld[0][2]), static_cast<float>(objectToWorld[1][2]),
                static_cast<float>(objectToWorld[2][2]), static_cast<float>(objectToWorld[3][2]),
            };

            instanceBuilder.defineInstance(transform, blasIndex, 0x01, blasGroupAccu,
                mcvr::faces::instanceFlags(*chunk1->geometryMaterialFlags, transform), chunk1->blas);

            hitGroupNames.push_back("shadow");
            for (int j = 0; j < chunk1->geometryCount; j++) {
                const std::string &groupName =
                    chunk1->geometryGroupNames != nullptr && j < static_cast<int>(chunk1->geometryGroupNames->size()) ?
                        (*chunk1->geometryGroupNames)[j] :
                        "default";
                hitGroupNames.push_back(groupName);
            }

            for (int j = 0; j < chunk1->geometryCount; j++) {
                indexBufferAddrs.push_back((*chunk1->indexBufferAddresses)[j]);
                positionBufferAddrs.push_back((*chunk1->positionBufferAddresses)[j]);
                materialBufferAddrs.push_back((*chunk1->materialBufferAddresses)[j]);
                auto appearance = identityAppearance();
                if (chunk1->geometryMaterialFlags != nullptr &&
                    j < static_cast<int>(chunk1->geometryMaterialFlags->size())) {
                    appearance.materialFlags = shaderMaterialFlags((*chunk1->geometryMaterialFlags)[j], *chunk1->geometryMaterialFlags);
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

            {
                glm::dmat4 previousObjectToWorld = objectToWorld;
                auto previousTransform = previousChunkTransformBatch.find(chunk1);
                if (chunk1->hasCustomTransform && previousTransform != previousChunkTransformBatch.end()) {
                    previousObjectToWorld = previousTransform->second;
                }
                glm::mat4 lastObjToWorldMat = glm::mat4(previousObjectToWorld);
                lastObjToWorldMats.push_back(lastObjToWorldMat);
            }

            blasOffset.push_back(blasAccu);
            blasAccu += chunk1->geometryCount;
            blasGroupAccu += chunk1->geometryCount + 1;

            blasIndex++;
        }
    }

    if (instanceBuilder.instances.empty()) {
        tlas = nullptr;
        return;
    }

    if (scene && scene->batchTlas) {
        tlas = scene->batchTlas;
    } else {
    tlas = instanceBuilder.endInstanceBuilder(device, vma)
               ->defineBuildProperty(VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR)
               ->querySizeInfo(device)
               ->allocateBuffers(physicalDevice, device, vma)
               ->buildAndSubmit(device, worldCommandBuffer);

        if (scene) { scene->batchTlas = tlas; ++scene->batchTlasBuilds; }
    }

    worldCommandBuffer->barriersMemory({vk::CommandBuffer::MemoryBarrier{
        .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    }});

    uploadBuffer(blasOffset, indexBufferAddrs, positionBufferAddrs, materialBufferAddrs,
                 lastIndexBufferAddrs, lastPositionBufferAddrs, lastObjToWorldMats,
                 instanceAppearances);
}

void WorldPrepareContext::setupHitGroupSbt(const std::unordered_map<std::string, uint32_t> &hitGroupNameToIndex,
                                           uint32_t fallbackHitGroupIndex,
                                           uint32_t shadowHitGroupIndex,
                                           std::shared_ptr<vk::CommandBuffer> commandBuffer,
                                           std::shared_ptr<vk::SBT> updateSbt,
                                           std::shared_ptr<vk::SBT> querySbt) {
    std::vector<uint32_t> hitGroupIndices;
    hitGroupIndices.reserve(hitGroupNames.size());

    for (const std::string &groupName : hitGroupNames) {
        if (groupName == "shadow") {
            hitGroupIndices.push_back(shadowHitGroupIndex);
            continue;
        }

        auto iter = hitGroupNameToIndex.find(groupName);
        hitGroupIndices.push_back(iter == hitGroupNameToIndex.end() ? fallbackHitGroupIndex : iter->second);
    }

    if (updateSbt != nullptr) { updateSbt->setupHitSBT(hitGroupIndices, commandBuffer); }
    if (querySbt != nullptr) { querySbt->setupHitSBT(hitGroupIndices, commandBuffer); }
}
