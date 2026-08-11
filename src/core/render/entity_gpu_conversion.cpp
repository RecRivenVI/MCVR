#include "entity_gpu_conversion.hpp"
#include "entity_conversion_commands.hpp"
#include "entities.hpp"
#include "render_framework.hpp"
#include "renderer.hpp"
#include "core/diagnostics/frame_profile.hpp"

#include <cstring>
#include <limits>

namespace {
std::shared_ptr<vk::DescriptorTable> makeEntityConversionTable(
    const std::shared_ptr<vk::DeviceLocalBuffer> &input,
    const std::shared_ptr<vk::DeviceLocalBuffer> &jobs,
    const std::shared_ptr<vk::DeviceLocalBuffer> &positions,
    const std::shared_ptr<vk::DeviceLocalBuffer> &materials,
    const std::shared_ptr<vk::DeviceLocalBuffer> &indices,
    const std::shared_ptr<vk::Device> &device) {
    auto table = vk::DescriptorTableBuilder{}.beginDescriptorLayoutSet().beginDescriptorLayoutSetBinding()
        .defineDescriptorLayoutSetBinding({0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
        .defineDescriptorLayoutSetBinding({1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
        .defineDescriptorLayoutSetBinding({2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
        .defineDescriptorLayoutSetBinding({3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
        .defineDescriptorLayoutSetBinding({4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
        .endDescriptorLayoutSetBinding().endDescriptorLayoutSet()
        .definePushConstant({VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)}).build(device);
    table->bindBuffer(input, 0, 0)->bindBuffer(jobs, 0, 1)->bindBuffer(positions, 0, 2)
        ->bindBuffer(materials, 0, 3)->bindBuffer(indices, 0, 4);
    return table;
}

size_t addEntityInputBytes(size_t total, size_t bytes) {
    if (bytes > std::numeric_limits<size_t>::max() - total)
        throw std::length_error("Entity conversion input size overflow");
    return total + bytes;
}

size_t deferredSourceBytes(const mcvr::EntityRawGeometry &raw) {
    if (raw.wordCount > std::numeric_limits<size_t>::max() / sizeof(uint32_t))
        throw std::length_error("Entity deferred source byte count overflow");
    const auto stride = mcvr::pbrSourceWords(raw.parameters.sourceVersion,raw.parameters.sourceWords) * 4;
    return mcvr::checkedEntitySourceBytes(raw.parameters.vertexCount,stride,
        static_cast<size_t>(raw.wordCount) * sizeof(uint32_t));
}
} // namespace

EntityGpuConversion::EntityGpuConversion(Framework &f,
    const std::vector<std::shared_ptr<EntityBuildData>> &data,
    std::shared_ptr<vk::DeviceLocalBuffer> positions, std::shared_ptr<vk::DeviceLocalBuffer> materials,
    std::shared_ptr<vk::DeviceLocalBuffer> indices, std::shared_ptr<vk::ComputePipeline> &pipeline) {
    mcvr::profile::Phases phase("entity.gpu-input-layout");
    std::vector<mcvr::EntitySourceJobGroup> groupedJobs;
    std::vector<std::shared_ptr<mcvr::EntityDirectInputBatch>> groupOwners;
    size_t fallbackBytes = 0;
    uint64_t vertexOffset = 0, indexOffset = 0, deferredVertices = 0, deferredBytes = 0;

    auto groupIndex = [&](std::uintptr_t key) {
        const size_t index = mcvr::entitySourceJobGroupIndex(groupedJobs, key);
        if (groupOwners.size() <= index) groupOwners.resize(index + 1);
        return index;
    };

    for (const auto &entity : data) for (size_t g = 0; g < entity->geometryCount; ++g) {
        const auto *raw = entity->rawGeometry.empty() ? nullptr : &entity->rawGeometry[g];
        const bool deferred = raw && raw->present();
        const bool directSource = deferred && raw->directInput != nullptr;
        if (deferred && !directSource && raw->words == nullptr)
            throw std::logic_error("Deferred entity source has no owned input");

        auto job = deferred ? raw->parameters : mcvr::EntityConvertJob{};
        const auto *topologyFlags=entity->topologyVertexFlags.size()>g ? &entity->topologyVertexFlags[g] : nullptr;
        const bool hasFlags=topologyFlags && !topologyFlags->empty();
        if(hasFlags && topologyFlags->size()!=entity->vertexCount(g)) throw std::logic_error("Entity topology flag count");
        if(hasFlags && directSource) throw std::logic_error("Deferred direct geometry unexpectedly has topology overrides");
        job.hasPositionFlags=hasFlags?1u:0u;
        job.vertexCount = entity->vertexCount(g);
        job.indexCount = entity->indexCount(g);
        if (vertexOffset > UINT32_MAX || indexOffset > UINT32_MAX ||
            job.vertexCount > UINT32_MAX - vertexOffset || job.indexCount > UINT32_MAX - indexOffset)
            throw std::length_error("Entity GPU output address overflow");
        job.vertexOffset = static_cast<uint32_t>(vertexOffset);
        job.indexOffset = static_cast<uint32_t>(indexOffset);
        job.emissiveOverlay = entity->emissiveOverlayTextureIDs[g];

        size_t sourceBytes = deferred ? deferredSourceBytes(*raw)
            : mcvr::entitySourceByteCount(job.vertexCount, sizeof(vk::VertexFormat::PBRVertex));
        if(deferred) deferredBytes=addEntityInputBytes(deferredBytes,sourceBytes);
        if (directSource) {
            const auto &owner = raw->directInput;
            if (!owner->buffer || owner->byteCount != owner->buffer->size())
                throw std::logic_error("Entity direct input owner has no matching buffer");
            if (raw->sourceWordOffset > mcvr::entityWordCount(owner->byteCount) ||
                raw->wordCount > mcvr::entityWordCount(owner->byteCount) - raw->sourceWordOffset ||
                sourceBytes != static_cast<size_t>(raw->wordCount) * sizeof(uint32_t))
                throw std::length_error("Entity direct source range exceeds its owned input");
            const uint64_t indexSource = static_cast<uint64_t>(raw->sourceWordOffset) + raw->wordCount;
            if (indexSource > UINT32_MAX)
                throw std::length_error("Entity direct source word offset overflow");
            job.source = raw->sourceWordOffset;
            job.indexSource = static_cast<uint32_t>(indexSource);
            const size_t group = groupIndex(reinterpret_cast<std::uintptr_t>(owner.get()));
            if (groupOwners[group] && groupOwners[group] != owner)
                throw std::logic_error("Entity source group key collision");
            groupOwners[group] = owner;
            mcvr::appendEntityTiles(groupedJobs[group].tiles, job);
            deferredVertices += job.vertexCount;
        } else {
            const size_t group = groupIndex(0);
            job.source = mcvr::entityWordCount(fallbackBytes);
            fallbackBytes = addEntityInputBytes(fallbackBytes, sourceBytes);
            job.indexSource = mcvr::entityWordCount(fallbackBytes);
            if (!deferred) {
                const size_t indexCount = entity->indices[g].size();
                if (indexCount > std::numeric_limits<size_t>::max() / sizeof(uint32_t))
                    throw std::length_error("Entity conversion index byte count overflow");
                fallbackBytes = addEntityInputBytes(fallbackBytes, indexCount * sizeof(uint32_t));
            } else {
                deferredVertices += job.vertexCount;
            }
            job.positionFlagsSource=mcvr::entityWordCount(fallbackBytes);
            if(hasFlags) fallbackBytes=addEntityInputBytes(fallbackBytes,topologyFlags->size()*sizeof(uint32_t));
            mcvr::appendEntityTiles(groupedJobs[group].tiles, job);
        }
        vertexOffset += job.vertexCount;
        indexOffset += job.indexCount;
    }

    const size_t jobStride = sizeof(mcvr::EntityConvertJob);
    const auto limit = static_cast<size_t>(f.physicalDevice()->properties().limits.maxStorageBufferRange);
    if (fallbackBytes > limit || positions->size() > limit || materials->size() > limit || indices->size() > limit)
        throw std::length_error("Entity GPU conversion exceeds storage buffer range");
    size_t uploadBytes = fallbackBytes;
    for (size_t i = 0; i < groupedJobs.size(); ++i) {
        const size_t tileCount = groupedJobs[i].tiles.size();
        if (tileCount > std::numeric_limits<size_t>::max() / jobStride)
            throw std::length_error("Entity conversion job byte count overflow");
        const size_t bytes = tileCount * jobStride;
        if (bytes > limit) throw std::length_error("Entity conversion jobs exceed storage buffer range");
        uploadBytes = addEntityInputBytes(uploadBytes, bytes);
        if (groupOwners[i]) {
            if (groupOwners[i]->byteCount > limit)
                throw std::length_error("Entity direct input exceeds storage buffer range");
            uploadBytes = addEntityInputBytes(uploadBytes, groupOwners[i]->byteCount);
        }
    }
    if (groupedJobs.empty()) throw std::length_error("Entity GPU conversion has no source jobs");

    phase.next("entity.gpu-input-copy");
    std::shared_ptr<vk::DeviceLocalBuffer> fallbackInput;
    if (fallbackBytes) {
        fallbackInput = vk::DeviceLocalBuffer::create(f.vma(), f.device(), false, fallbackBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        fallbackInput->writeToStagingBuffer([&](void *mapped, size_t mappedBytes) {
            if (mappedBytes < fallbackBytes)
                throw std::length_error("Entity fallback staging is shorter than its source data");
            auto *target = static_cast<std::byte *>(mapped);
            size_t cursor = 0;
            for (const auto &entity : data) for (size_t g = 0; g < entity->geometryCount; ++g) {
                const auto *raw = entity->rawGeometry.empty() ? nullptr : &entity->rawGeometry[g];
                const bool deferred = raw && raw->present();
                const bool directSource = deferred && raw->directInput != nullptr;
                if (directSource) continue;

                const size_t vertexBytes = deferred ? deferredSourceBytes(*raw)
                    : mcvr::entitySourceByteCount(entity->vertexCount(g), sizeof(vk::VertexFormat::PBRVertex));
                if (cursor > fallbackBytes || vertexBytes > fallbackBytes - cursor)
                    throw std::length_error("Entity fallback source exceeds its input allocation");
                const void *source = deferred ? static_cast<const void *>(raw->words.get())
                    : static_cast<const void *>(entity->vertices[g].data());
                if (vertexBytes && source == nullptr)
                    throw std::logic_error("Entity fallback source address is null");
                if (vertexBytes) std::memcpy(target + cursor, source, vertexBytes);
                cursor += vertexBytes;
                if (!deferred) {
                    const auto &geometryIndices = entity->indices[g];
                    const size_t indexBytes = geometryIndices.size() * sizeof(uint32_t);
                    if (cursor > fallbackBytes || indexBytes > fallbackBytes - cursor)
                        throw std::length_error("Entity fallback indices exceed their input allocation");
                    if (indexBytes) std::memcpy(target + cursor, geometryIndices.data(), indexBytes);
                    cursor += indexBytes;
                }
                if(entity->topologyVertexFlags.size()>g && !entity->topologyVertexFlags[g].empty()) {
                    const auto &flags=entity->topologyVertexFlags[g];
                    const size_t bytes=flags.size()*sizeof(uint32_t);
                    if(cursor>fallbackBytes || bytes>fallbackBytes-cursor) throw std::length_error("Entity topology input range");
                    std::memcpy(target+cursor,flags.data(),bytes);cursor+=bytes;
                }
            }
            if (cursor != fallbackBytes)
                throw std::logic_error("Entity fallback source layout did not match its allocation");
        });
    }

    auto device = f.device();
    sourceGroups.reserve(groupedJobs.size());
    for (size_t i = 0; i < groupedJobs.size(); ++i) {
        auto &planned = groupedJobs[i];
        SourceGroup group;
        group.directOwner = groupOwners[i];
        group.input = group.directOwner ? group.directOwner->buffer : fallbackInput;
        if (!group.input || planned.tiles.empty())
            throw std::logic_error("Entity source group is incomplete");
        group.tiles = static_cast<uint32_t>(planned.tiles.size());
        const size_t jobBytes = planned.tiles.size() * sizeof(mcvr::EntityConvertJob);
        group.jobs = vk::DeviceLocalBuffer::create(f.vma(), device, false, jobBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        group.jobs->uploadToStagingBuffer(planned.tiles.data());
        group.table = makeEntityConversionTable(group.input, group.jobs, positions, materials, indices, device);
        sourceGroups.push_back(std::move(group));
    }

    phase.next("entity.gpu-descriptors");
    if (!pipeline) {
        auto shader = vk::Shader::create(device,
            (Renderer::folderPath / "shaders/world/entity_convert_comp.spv").string());
        pipeline = vk::ComputePipelineBuilder{}.defineShader(shader)
            .definePipelineLayout(sourceGroups.front().table).build(device);
    }
    pipeline_ = pipeline;
    if (mcvr::profile::enabled()) {
        mcvr::profile::emit(mcvr::profile::frame, 5, "entity.gpu-deferred", deferredVertices,
            deferredBytes);
        mcvr::profile::emit(mcvr::profile::frame, 5, "entity.gpu-upload",
            sourceGroups.size() * 2, uploadBytes);
        mcvr::profile::emit(mcvr::profile::frame, 5, "entity.gpu-output", 3,
            positions->size() + materials->size() + indices->size());
    }
}

void EntityGpuConversion::record(Framework &f, const std::shared_ptr<vk::CommandBuffer> &commands) {
    if (recorded_) return;
    // Keep direct per-submission source owners and every descriptor/job buffer until completion.
    f.frameResourceRetainer().retain(shared_from_this());
    auto context = f.safeAcquireCurrentContext();
    const auto stamp = context->auditGpu.begin(commands->vkCommandBuffer(), "entity-convert-gpu");
    for (const auto &group : sourceGroups) {
        mcvr::recordEntityConversion(commands->vkCommandBuffer(), pipeline_->vkPipeline(),
            group.table->vkPipelineLayout(), group.table->descriptorSet().at(0), group.tiles,
            f.physicalDevice()->properties().limits.maxComputeWorkGroupCount[0],
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
    }
    context->auditGpu.end(commands->vkCommandBuffer(), stamp);
    recorded_ = true;
}
