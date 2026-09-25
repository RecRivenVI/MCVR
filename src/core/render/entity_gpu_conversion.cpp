#include "entity_gpu_conversion.hpp"
#include "entity_conversion_commands.hpp"
#include "entities.hpp"
#include "render_framework.hpp"
#include "renderer.hpp"
#include "core/diagnostics/frame_profile.hpp"
#include <cstring>

EntityGpuConversion::EntityGpuConversion(Framework &f,
    const std::vector<std::shared_ptr<EntityBuildData>> &data,
    std::shared_ptr<vk::DeviceLocalBuffer> positions, std::shared_ptr<vk::DeviceLocalBuffer> materials,
    std::shared_ptr<vk::DeviceLocalBuffer> indices, std::shared_ptr<vk::ComputePipeline> &pipeline) {
    mcvr::profile::Phases phase("entity.gpu-input-layout");
    std::vector<mcvr::EntityConvertJob> tiles;
    size_t words = 0;
    uint64_t vertexOffset = 0, indexOffset = 0, deferredVertices = 0;
    for (const auto &entity : data) for (size_t g = 0; g < entity->geometryCount; ++g) {
        const auto *raw = entity->rawGeometry.empty() ? nullptr : &entity->rawGeometry[g];
        const bool deferred = raw && raw->present();
        auto job = deferred ? raw->parameters : mcvr::EntityConvertJob{};
        job.source = mcvr::entityWordCount(words * 4);
        job.vertexCount = entity->vertexCount(g); job.indexCount = entity->indexCount(g);
        if (vertexOffset + job.vertexCount > UINT32_MAX || indexOffset + job.indexCount > UINT32_MAX)
            throw std::length_error("Entity GPU output address overflow");
        job.vertexOffset = static_cast<uint32_t>(vertexOffset); job.indexOffset = static_cast<uint32_t>(indexOffset);
        job.emissiveOverlay = entity->emissiveOverlayTextureIDs[g];
        words += static_cast<size_t>(job.vertexCount) * (sizeof(vk::VertexFormat::PBRVertex) / 4);
        job.indexSource = mcvr::entityWordCount(words * 4);
        if (!deferred) words += job.indexCount;
        else deferredVertices += job.vertexCount;
        mcvr::appendEntityTiles(tiles, job);
        vertexOffset += job.vertexCount; indexOffset += job.indexCount;
    }
    const size_t inputBytes = words * 4, jobBytes = tiles.size() * sizeof(tiles[0]);
    const auto limit = f.physicalDevice()->properties().limits.maxStorageBufferRange;
    if (!inputBytes || !jobBytes || inputBytes > limit || jobBytes > limit ||
        positions->size() > limit || materials->size() > limit || indices->size() > limit)
        throw std::length_error("Entity GPU conversion exceeds storage buffer range");
    tiles_ = static_cast<uint32_t>(tiles.size());
    phase.next("entity.gpu-input-copy");
    input = vk::DeviceLocalBuffer::create(f.vma(), f.device(), false, inputBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    input->writeToStagingBuffer([&](void *mapped, size_t) {
        auto *target = static_cast<std::byte *>(mapped);
        for (const auto &entity : data) for (size_t g = 0; g < entity->geometryCount; ++g) {
            const auto *raw = entity->rawGeometry.empty() ? nullptr : &entity->rawGeometry[g];
            const bool deferred = raw && raw->present();
            const size_t bytes = static_cast<size_t>(entity->vertexCount(g)) * sizeof(vk::VertexFormat::PBRVertex);
            const void *source = deferred ? static_cast<const void *>(raw->words.get()) : entity->vertices[g].data();
            if (bytes) std::memcpy(target, source, bytes);
            target += bytes;
            if (!deferred) {
                const size_t indexBytes = entity->indices[g].size() * sizeof(uint32_t);
                if (indexBytes) std::memcpy(target, entity->indices[g].data(), indexBytes);
                target += indexBytes;
            }
        }
    });
    jobs = vk::DeviceLocalBuffer::create(f.vma(), f.device(), false, jobBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    jobs->uploadToStagingBuffer(tiles.data());
    phase.next("entity.gpu-descriptors");
    table_ = vk::DescriptorTableBuilder{}.beginDescriptorLayoutSet().beginDescriptorLayoutSetBinding()
        .defineDescriptorLayoutSetBinding({0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
        .defineDescriptorLayoutSetBinding({1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
        .defineDescriptorLayoutSetBinding({2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
        .defineDescriptorLayoutSetBinding({3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
        .defineDescriptorLayoutSetBinding({4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
        .endDescriptorLayoutSetBinding().endDescriptorLayoutSet()
        .definePushConstant({VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)}).build(f.device());
    table_->bindBuffer(input, 0, 0)->bindBuffer(jobs, 0, 1)->bindBuffer(positions, 0, 2)
        ->bindBuffer(materials, 0, 3)->bindBuffer(indices, 0, 4);
    if (!pipeline) {
        auto shader = vk::Shader::create(f.device(), (Renderer::folderPath / "shaders/world/entity_convert_comp.spv").string());
        pipeline = vk::ComputePipelineBuilder{}.defineShader(shader).definePipelineLayout(table_).build(f.device());
    }
    pipeline_ = pipeline;
    if (mcvr::profile::enabled()) {
        mcvr::profile::emit(mcvr::profile::frame, 5, "entity.gpu-deferred", deferredVertices, deferredVertices * 128);
        mcvr::profile::emit(mcvr::profile::frame, 5, "entity.gpu-upload", 2, inputBytes + jobBytes);
        mcvr::profile::emit(mcvr::profile::frame, 5, "entity.gpu-output", 3, positions->size()+materials->size()+indices->size());
    }
}

void EntityGpuConversion::record(Framework &f, const std::shared_ptr<vk::CommandBuffer> &commands) {
    if (recorded_) return;
    // Retain before recording, including failure/unwind paths. No descriptor is rewritten.
    f.frameResourceRetainer().retain(shared_from_this());
    auto context = f.safeAcquireCurrentContext();
    const auto stamp = context->auditGpu.begin(commands->vkCommandBuffer(), "entity-convert-gpu");
    mcvr::recordEntityConversion(commands->vkCommandBuffer(), pipeline_->vkPipeline(), table_->vkPipelineLayout(),
        table_->descriptorSet().at(0), tiles_, f.physicalDevice()->properties().limits.maxComputeWorkGroupCount[0],
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
    context->auditGpu.end(commands->vkCommandBuffer(), stamp);
    recorded_ = true;
}
