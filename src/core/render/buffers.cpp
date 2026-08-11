

#include "core/logging.hpp"
#include "core/render/buffers.hpp"

#include "common/shared.hpp"
#include "core/render/chunks.hpp"
#include "core/render/index_patterns.hpp"
#include "core/render/pending_uploads.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/world.hpp"

#include <algorithm>
#include <cstring>
#include <random>

auto buffersCout() {
    return mcvr::log::info("Buffers");
}

auto buffersCerr() {
    return mcvr::log::error("Buffers");
}

Buffers::Buffers(std::shared_ptr<Framework> framework) {
    // Offscreen scene construction can upload geometry before its first frame reset.
    importantIndexVertexBuffer_ = std::make_shared<std::vector<std::shared_ptr<vk::DeviceLocalBuffer>>>();
    uint32_t size = framework->recordingContextCount();
    auto device = framework->device();
    auto vma = framework->vma();
    auto alignTo = [](uint32_t value, uint32_t alignment) {
        if (alignment <= 1 || value == 0) return value;
        return ((value + alignment - 1) / alignment) * alignment;
    };
    overlayDrawUniformAlignment_ =
        std::max<uint32_t>(1, framework->physicalDevice()->properties().limits.minUniformBufferOffsetAlignment);
    overlayDrawUniformDeviceLimit_ =
        std::max<uint32_t>(1, framework->physicalDevice()->properties().limits.maxUniformBufferRange);
    overlayDrawUniformDescriptorRange_ =
        std::min<uint32_t>(overlayDrawUniformDeviceLimit_, overlayDrawUniformInitialDescriptorRange);
    overlayPostUniformDescriptorRange_ = sizeof(vk::Data::OverlayPostUBO);
    overlayPostUniformStride_ = alignTo(overlayPostUniformDescriptorRange_, overlayDrawUniformAlignment_);

    validOverlayIndex_.resize(size);
    overlayIndexVertexBuffer_.resize(size);

    overlayDrawUniformBuffer_.resize(size);
    overlayPostUniformBuffer_.resize(size);
    overlayDrawUniformData_.resize(size);
    overlayDrawUniformWriteOffset_.resize(size, 0);
    overlayPostUniformData_.resize(size);
    overlayPostUniformCount_.resize(size, 0);

    worldUniformBuffer_.resize(size);
    lastWorldUniformBuffer_.resize(size);
    skyUniformBuffer_.resize(size);
    textureMappingBuffer_.resize(size);
    exposureDataBuffer_.resize(size);

    for (uint32_t i = 0; i < size; i++) {
        overlayDrawUniformBuffer_[i] = vk::HostVisibleBuffer::create(
            vma, device, std::max<uint32_t>(overlayDrawUniformInitialSize, overlayDrawUniformAlignment_),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        overlayDrawUniformData_[i].reserve(overlayDrawUniformBuffer_[i]->size());
        overlayPostUniformBuffer_[i] = vk::HostVisibleBuffer::create(
            vma, device, std::max<uint32_t>(overlayPostUniformInitialSize, overlayPostUniformStride_),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        overlayPostUniformData_[i].reserve(overlayPostUniformBuffer_[i]->size());
    }
}

bool Buffers::ensureOverlayDrawUniformBufferCapacityLocked(std::shared_ptr<Framework> framework,
                                                           uint32_t frameIndex,
                                                           uint32_t requiredBufferSize) {
    auto &buffer = overlayDrawUniformBuffer_[frameIndex];
    if (requiredBufferSize <= buffer->size()) { return false; }

    uint32_t newSize = std::max<uint32_t>(buffer->size(), overlayDrawUniformInitialSize);
    while (newSize < requiredBufferSize) { newSize *= 2; }

    auto newBuffer =
        vk::HostVisibleBuffer::create(framework->vma(), framework->device(), newSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto recordingFrame = framework->safeAcquireCurrentContext();
    if (recordingFrame && recordingFrame->frameIndex == frameIndex && !recordingFrame->frameSubmitted) {
        auto &data = overlayDrawUniformData_[frameIndex];
        const auto bytes = std::min<size_t>(data.size(), buffer->size());
        if (bytes != 0) buffer->uploadToBuffer(data.data(), bytes, 0);
    }
    framework->frameResourceRetainer().retain(buffer);
    overlayDrawUniformBuffer_[frameIndex] = newBuffer;
    overlayDrawUniformData_[frameIndex].reserve(newSize);
    return true;
}

void Buffers::resetFrame() {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto framework = Renderer::instance().framework();
    auto context = framework->safeAcquireCurrentContext();
    auto &frr = framework->frameResourceRetainer();

    validOverlayIndex_[context->frameIndex].clear();

    overlayNextID_ = 0;

    frr.retain(importantIndexVertexBuffer_);
    importantIndexVertexBuffer_ = std::make_shared<std::vector<std::shared_ptr<vk::DeviceLocalBuffer>>>();

    overlayDrawUniformData_[context->frameIndex].clear();
    overlayDrawUniformWriteOffset_[context->frameIndex] = 0;
    overlayPostUniformData_[context->frameIndex].clear();
    overlayPostUniformCount_[context->frameIndex] = 0;
}

uint32_t Buffers::allocateBuffer() {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();

    validOverlayIndex_[context->frameIndex].insert(std::make_pair(overlayNextID_, -1));
    auto it = overlayIndexVertexBuffer_[context->frameIndex].find(overlayNextID_);
    if (it == overlayIndexVertexBuffer_[context->frameIndex].end()) {
        overlayIndexVertexBuffer_[context->frameIndex].emplace(std::make_pair(overlayNextID_, nullptr));
    }
    return overlayNextID_++;
}

uint32_t Buffers::allocatePersistentBuffer() {
    std::lock_guard lock(mtx_);
    if (nextPersistentId_ >= 0x7fffffffu) throw std::runtime_error("Persistent buffer names exhausted");
    uint32_t id = nextPersistentId_++;
    persistentBuffers_.emplace(id, PersistentBuffer{});
    return id;
}

void Buffers::releasePersistentBuffer(uint32_t id) {
    std::lock_guard lock(mtx_);
    auto found = persistentBuffers_.find(id);
    if (found == persistentBuffers_.end()) return;
    auto framework = Renderer::instance().framework();
    if (framework && framework->isRunning()) framework->frameResourceRetainer().retain(found->second.buffer);
    persistentBuffers_.erase(found);
}

void Buffers::initializeBuffer(uint32_t id, uint32_t size, VkBufferUsageFlags usageFlags) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto framework = Renderer::instance().framework();
    if (!framework || !framework->isRunning()) throw std::runtime_error("Vulkan framework is unavailable");
    auto device = framework->device();
    auto vma = framework->vma();

    if (id >= 0x40000000u) {
        auto found = persistentBuffers_.find(id);
        if (found == persistentBuffers_.end()) throw std::runtime_error("Unknown persistent buffer");
        auto &record = found->second;
        auto replacement = vk::DeviceLocalBuffer::create(vma, device, std::max(4u, size), usageFlags);
        framework->frameResourceRetainer().retain(record.buffer);
        record = {replacement, size, usageFlags, true, false};
        return;
    }

    auto context = framework->safeAcquireCurrentContext();
    if (!context) throw std::runtime_error("Buffer initialization requires an acquired frame");

    auto bufferIter = overlayIndexVertexBuffer_[context->frameIndex].find(id);
    if (!validOverlayIndex_[context->frameIndex].contains(id) ||
        bufferIter == overlayIndexVertexBuffer_[context->frameIndex].end()) {
        throw std::out_of_range("Overlay buffer id is not allocated: " + std::to_string(id));
    }

    validOverlayIndex_[context->frameIndex].at(id) = size;

    auto buffer = overlayIndexVertexBuffer_[context->frameIndex].contains(id) ?
                      overlayIndexVertexBuffer_[context->frameIndex].at(id) :
                      nullptr;
    uint32_t currentSize = buffer == nullptr ? baseBlockSize : buffer->size();
    while (currentSize < size) currentSize *= 2;
    if (buffer == nullptr || currentSize != buffer->size()) {
        framework->frameResourceRetainer().retain(buffer);
        overlayIndexVertexBuffer_[context->frameIndex].at(id) =
            vk::DeviceLocalBuffer::create(vma, device, currentSize, usageFlags);
    }
}

void Buffers::buildIndexBuffer(uint32_t dstId, int type, int drawMode, int vertexCount, int expectedIndexCount) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto buildFourVertexIndices = [this, dstId, vertexCount,
                                   expectedIndexCount](mcvr::render::FourVertexIndexPattern pattern) {
        auto indices = mcvr::render::buildFourVertexIndices<uint16_t>(vertexCount, expectedIndexCount, pattern);
        queueOverlayUpload(reinterpret_cast<uint8_t *>(indices.data()), dstId);
    };
    auto buildFourVertexIndices32 = [this, dstId, vertexCount,
                                     expectedIndexCount](mcvr::render::FourVertexIndexPattern pattern) {
        auto indices = mcvr::render::buildFourVertexIndices<uint32_t>(vertexCount, expectedIndexCount, pattern);
        queueOverlayUpload(reinterpret_cast<uint8_t *>(indices.data()), dstId);
    };
    auto buildSequentialIndices = [this, dstId, vertexCount, expectedIndexCount]<typename V>() {
        if (vertexCount != expectedIndexCount) { throw std::runtime_error("sequential index count not match!"); }

        std::vector<V> indices;
        indices.reserve(expectedIndexCount);
        for (int i = 0; i < expectedIndexCount; ++i) { indices.push_back(static_cast<V>(i)); }

        queueOverlayUpload(reinterpret_cast<uint8_t *>(indices.data()), dstId);
    };

    switch (drawMode) {
        case 0: {
            switch (type) {
                case 0: {
                    buildFourVertexIndices(mcvr::render::FourVertexIndexPattern::MINECRAFT_LINES);
                    break;
                }
                case 1: {
                    buildFourVertexIndices32(mcvr::render::FourVertexIndexPattern::MINECRAFT_LINES);
                    break;
                }
            }
            break;
        }

        case 7: {
            switch (type) {
                case 0: {
                    buildFourVertexIndices(mcvr::render::FourVertexIndexPattern::QUADS);
                    break;
                }
                case 1: {
                    buildFourVertexIndices32(mcvr::render::FourVertexIndexPattern::QUADS);
                    break;
                }
            }
            break;
        }

        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6: {
            switch (type) {
                case 0: {
                    buildSequentialIndices.template operator()<uint16_t>();
                    break;
                }
                case 1: {
                    buildSequentialIndices.template operator()<uint32_t>();
                    break;
                }
            }
            break;
        }

        default: {
            mcvr::log::info("Buffers") << "Get draw mode=" << drawMode << std::endl;
            throw std::runtime_error("not implemented yet");
        }
    }
}

void Buffers::queueOverlayUpload(uint8_t *srcPointer, uint32_t dstId) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    if (dstId >= 0x40000000u) {
        auto found = persistentBuffers_.find(dstId);
        if (found == persistentBuffers_.end() || !found->second.buffer)
            throw std::runtime_error("Upload requires initialized persistent buffer storage");
        auto &record = found->second;
        if (!record.uploadReady)
            throw std::runtime_error("Persistent buffer must be initialized before each upload");
        if (record.size > 0) {
            if (!srcPointer) throw std::invalid_argument("Null persistent buffer upload");
            record.buffer->uploadToStagingBuffer(srcPointer, record.size, 0);
            pendingPersistentUploads_.push_back(record.buffer);
            record.rangeWriteOpen = true;
        }
        record.uploadReady = false;
        return;
    }
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();
    auto buffer = overlayIndexVertexBuffer_[context->frameIndex].at(dstId);
    if (validOverlayIndex_[context->frameIndex].contains(dstId) && buffer != nullptr) {
        auto size = validOverlayIndex_[context->frameIndex].at(dstId);
        if (size > 0) { buffer->uploadToStagingBuffer(srcPointer, size, 0); }
    }
}

void Buffers::queuePersistentUploadRange(uint8_t *srcPointer, uint32_t size,
                                         uint32_t dstId, uint32_t dstOffset) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto found = persistentBuffers_.find(dstId);
    if (found == persistentBuffers_.end() || !found->second.buffer)
        throw std::runtime_error("Range upload requires initialized persistent buffer storage");
    auto &record = found->second;
    if (dstOffset > record.size || size > record.size - dstOffset)
        throw std::out_of_range("Persistent buffer range upload exceeds storage");
    if (size == 0) return;
    if (!srcPointer) throw std::invalid_argument("Null persistent buffer range upload");
    if (!record.rangeWriteOpen) {
        auto framework = Renderer::instance().framework();
        if (!framework || !framework->isRunning())
            throw std::runtime_error("Vulkan framework is unavailable for range upload");
        auto replacement = vk::DeviceLocalBuffer::create(framework->vma(), framework->device(),
            std::max(4u, record.size), record.usage);
        if (record.size != 0) {
            replacement->uploadToStagingBuffer(record.buffer->mappedPtr(), record.size, 0);
        }
        framework->frameResourceRetainer().retain(record.buffer);
        record.buffer = replacement;
        record.rangeWriteOpen = true;
    }
    record.buffer->uploadToStagingBuffer(srcPointer, size, dstOffset);
    if (std::find(pendingPersistentUploads_.begin(), pendingPersistentUploads_.end(),
                  record.buffer) == pendingPersistentUploads_.end()) {
        pendingPersistentUploads_.push_back(record.buffer);
    }
}

void Buffers::queueImportantWorldUpload(std::shared_ptr<vk::DeviceLocalBuffer> vertexBuffer,
                                        std::shared_ptr<vk::DeviceLocalBuffer> indexBuffer) {
    Renderer::instance().framework()->safeAcquireCurrentContext();
    Renderer::instance().framework()->safeAcquireCurrentContext();
    queueImportantWorldUpload(vertexBuffer);
    queueImportantWorldUpload(indexBuffer);
}

void Buffers::queueImportantWorldUpload(std::shared_ptr<vk::DeviceLocalBuffer> buffer) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    if (buffer == nullptr) return;
    importantIndexVertexBuffer_->push_back(buffer);
}

void Buffers::performQueuedUpload() {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto uploads = mcvr::takePendingUploads(importantIndexVertexBuffer_);
    Renderer::instance().framework()->frameResourceRetainer().retain(uploads);
    auto frameIndex = Renderer::instance().framework()->safeAcquireCurrentContext()->frameIndex;
    std::shared_ptr<vk::CommandBuffer> cmdBuffer =
        Renderer::instance().framework()->safeAcquireCurrentContext()->uploadCommandBuffer;

    auto physicalDevice = Renderer::instance().framework()->physicalDevice();
    auto mainQueueIndex = physicalDevice->mainQueueIndex();

    std::vector<vk::CommandBuffer::BufferMemoryBarrier> uploadPreBufferBarriers, uploadPostBufferBarriers;

    for (auto [bufferId, size] : validOverlayIndex_[frameIndex]) {
        auto buffer = overlayIndexVertexBuffer_[frameIndex].at(bufferId);
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
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                            VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .buffer = buffer,
        });
    }

    for (auto buffer : *uploads) {
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
                            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                            VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .buffer = buffer,
        });
    }

    for (const auto &buffer : pendingPersistentUploads_) {
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
            .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                            VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                            VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                             VK_ACCESS_2_MEMORY_READ_BIT,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .buffer = buffer,
        });
    }

    cmdBuffer->barriersBufferImage(uploadPreBufferBarriers, {});

    for (auto [bufferId, size] : validOverlayIndex_[frameIndex]) {
        auto buffer = overlayIndexVertexBuffer_[frameIndex].at(bufferId);
        if (size > 0) { buffer->uploadToBuffer(cmdBuffer, size, 0, 0); }
    }

    for (auto buffer : *uploads) { buffer->uploadToBuffer(cmdBuffer); }

    auto &retainer = Renderer::instance().framework()->frameResourceRetainer();
    for (const auto &buffer : pendingPersistentUploads_) {
        buffer->uploadToBuffer(cmdBuffer);
        retainer.retain(buffer);
    }
    for (auto &[id, record] : persistentBuffers_) record.rangeWriteOpen = false;
    pendingPersistentUploads_.clear();

    cmdBuffer->barriersBufferImage(uploadPostBufferBarriers, {});
}

void Buffers::appendOverlayDrawUniform(uint8_t *srcPointer, uint32_t size, uint32_t &uniformOffset) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto framework = Renderer::instance().framework();
    auto context = framework->safeAcquireCurrentContext();
    auto frameIndex = context->frameIndex;
    auto alignTo = [](uint32_t value, uint32_t alignment) {
        if (alignment <= 1 || value == 0) return value;
        return ((value + alignment - 1) / alignment) * alignment;
    };

    uniformOffset = overlayDrawUniformWriteOffset_[frameIndex];
    uint32_t alignedSize = alignTo(size, overlayDrawUniformAlignment_);
    if (size > overlayDrawUniformDescriptorRange_) {
        throw std::runtime_error("Overlay draw uniform exceeds fixed descriptor range");
    }

    uint32_t requiredSize = uniformOffset + size;
    auto &cpuData = overlayDrawUniformData_[frameIndex];
    if (cpuData.size() < requiredSize) { cpuData.resize(requiredSize, 0); }

    if (size > 0) { std::memcpy(cpuData.data() + uniformOffset, srcPointer, size); }

    overlayDrawUniformWriteOffset_[frameIndex] = uniformOffset + alignedSize;

    if (ensureOverlayDrawUniformBufferCapacityLocked(framework, frameIndex,
                                                     uniformOffset + overlayDrawUniformDescriptorRange_)) {
        auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
        pipelineContext->uiModuleContext->refreshOverlayDescriptorTable();
    }
}

bool Buffers::registerOverlayDrawUniformSize(uint32_t size) {
    auto framework = Renderer::instance().framework();
    if (framework == nullptr) return false;

    {
        std::unique_lock<std::recursive_mutex> lck(mtx_);
        uint32_t requiredRange = std::max<uint32_t>(1, size);
        if (requiredRange > overlayDrawUniformDeviceLimit_) {
            throw std::runtime_error("Overlay draw uniform exceeds device maxUniformBufferRange");
        }

        if (requiredRange <= overlayDrawUniformDescriptorRange_) { return false; }

        overlayDrawUniformDescriptorRange_ = requiredRange;
        for (uint32_t frameIndex = 0; frameIndex < overlayDrawUniformBuffer_.size(); ++frameIndex) {
            ensureOverlayDrawUniformBufferCapacityLocked(framework, frameIndex, requiredRange);
        }
    }

    return true;
}

void Buffers::appendOverlayPostUniform(vk::Data::OverlayPostUBO &ubo) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto framework = Renderer::instance().framework();
    auto context = framework->safeAcquireCurrentContext();
    if (context->frameSubmitted) throw std::logic_error("Cannot append post uniforms after frame submission");
    auto frameIndex = context->frameIndex;

    uint32_t uniformOffset = overlayPostUniformCount_[frameIndex] * overlayPostUniformStride_;
    uint32_t requiredSize = uniformOffset + overlayPostUniformDescriptorRange_;
    auto &cpuData = overlayPostUniformData_[frameIndex];
    if (cpuData.size() < requiredSize) { cpuData.resize(requiredSize, 0); }

    std::memcpy(cpuData.data() + uniformOffset, &ubo, sizeof(vk::Data::OverlayPostUBO));

    auto &buffer = overlayPostUniformBuffer_[frameIndex];
    if (uniformOffset + overlayPostUniformDescriptorRange_ > buffer->size()) {
        uint32_t requiredBufferSize = uniformOffset + overlayPostUniformDescriptorRange_;
        uint32_t newSize = std::max<uint32_t>(buffer->size(), overlayPostUniformInitialSize);
        while (newSize < requiredBufferSize) { newSize *= 2; }

        auto newBuffer =
            vk::HostVisibleBuffer::create(framework->vma(), framework->device(), newSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        // Earlier draws retain this descriptor/buffer generation until submission.
        const auto retiredBytes = std::min<size_t>(cpuData.size(), buffer->size());
        if (retiredBytes != 0) buffer->uploadToBuffer(cpuData.data(), retiredBytes, 0);
        framework->frameResourceRetainer().retain(buffer);
        overlayPostUniformBuffer_[frameIndex] = newBuffer;
        cpuData.reserve(newSize);

        auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
        pipelineContext->uiModuleContext->refreshOverlayDescriptorTable();
    }

    overlayPostUniformCount_[frameIndex]++;
}

vk::Data::OverlayPostUBO Buffers::recordedOverlayPostUniform(uint32_t offset) {
    std::lock_guard lock(mtx_);
    auto frame = Renderer::instance().framework()->safeAcquireCurrentContext();
    const auto &data = overlayPostUniformData_.at(frame->frameIndex);
    if (offset > data.size() || data.size() - offset < sizeof(vk::Data::OverlayPostUBO))
        throw std::out_of_range("Post uniform has not been recorded");
    vk::Data::OverlayPostUBO result;
    std::memcpy(&result, data.data() + offset, sizeof(result));
    return result;
}

void Buffers::buildAndUploadOverlayUniformBuffer() {
    auto framework = Renderer::instance().framework();
    auto context = framework->safeAcquireCurrentContext();
    auto frameIndex = context->frameIndex;

    auto &drawCpuData = overlayDrawUniformData_[frameIndex];
    auto &drawBuffer = overlayDrawUniformBuffer_[frameIndex];
    if (!drawCpuData.empty()) {
        if (drawCpuData.size() > drawBuffer->size()) {
            throw std::runtime_error("Overlay draw uniform buffer exhausted before upload");
        }
        drawBuffer->uploadToBuffer(drawCpuData.data(), drawCpuData.size(), 0);
    }

    auto &postCpuData = overlayPostUniformData_[frameIndex];
    auto &postBuffer = overlayPostUniformBuffer_[frameIndex];
    if (!postCpuData.empty()) {
        if (postCpuData.size() > postBuffer->size()) {
            throw std::runtime_error("Overlay post uniform buffer exhausted before upload");
        }
        postBuffer->uploadToBuffer(postCpuData.data(), postCpuData.size(), 0);
    }
}


// halton low discrepancy sequence, from https://www.shadertoy.com/view/wdXSW8
glm::vec2 halton(int index) {
    const glm::vec2 coprimes = glm::vec2(2.0F, 3.0F);
    glm::vec2 s = glm::vec2(index, index);
    glm::vec4 a = glm::vec4(1, 1, 0, 0);
    while (s.x > 0. && s.y > 0.) {
        a.x = a.x / coprimes.x;
        a.y = a.y / coprimes.y;
        a.z += a.x * fmod(s.x, coprimes.x);
        a.w += a.y * fmod(s.y, coprimes.y);
        s.x = floorf(s.x / coprimes.x);
        s.y = floorf(s.y / coprimes.y);
    }
    return glm::vec2(a.z, a.w);
}

void Buffers::setAndUploadWorldUniformBuffer(vk::Data::WorldUBO &ubo) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto framework = Renderer::instance().framework();
    auto context = framework->safeAcquireCurrentContext();
    auto vma = framework->vma();
    auto device = framework->device();

    glm::mat4 mapGLToVulkan(1.0f);
    mapGLToVulkan[1][1] = -1.0f;
    mapGLToVulkan[2][2] = 0.5f;
    mapGLToVulkan[3][2] = 0.5f;

    ubo.cameraProjMat = mapGLToVulkan * ubo.cameraProjMat;

    ubo.cameraViewMatInv = glm::inverse(ubo.cameraViewMat);
    ubo.cameraEffectedViewMatInv = glm::inverse(ubo.cameraEffectedViewMat);
    ubo.cameraProjMatInv = glm::inverse(ubo.cameraProjMat);

    {
        static std::random_device seed;
        static std::ranlux48 engine(seed());
        static std::uniform_int_distribution<> distrib;
        ubo.seed = distrib(engine);
    }

    ubo.cameraJitter = useJitter_ ? halton(jitterSequenceIndex_++) - glm::vec2(0.5) : glm::vec2(0.0);

    auto world = Renderer::instance().world();
    ubo.cameraPos.x = world->getCameraPos().x;
    ubo.cameraPos.y = world->getCameraPos().y;
    ubo.cameraPos.z = world->getCameraPos().z;
    ubo.cameraPos.w = 0;
    if (auto chunks = world->chunks(); chunks != nullptr) {
        ubo.chunkGridInfo = chunks->chunkGridInfo();
        ubo.chunkStorageSectionPos = chunks->chunkStorageSectionPos();
    } else {
        ubo.chunkGridInfo = glm::ivec4(0);
        ubo.chunkStorageSectionPos = glm::ivec4(0);
    }

    if (worldUniformBuffer_[context->frameIndex] == nullptr) {
        worldUniformBuffer_[context->frameIndex] =
            vk::HostVisibleBuffer::create(vma, device, sizeof(vk::Data::WorldUBO),
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    if (lastWorldUniformBuffer_[context->frameIndex] == nullptr) {
        lastWorldUniformBuffer_[context->frameIndex] =
            vk::HostVisibleBuffer::create(vma, device, sizeof(vk::Data::WorldUBO),
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }

    worldUniformBuffer_[context->frameIndex]->uploadToBuffer(&ubo);
    const bool hasHistory = worldHistoryValid_.exchange(true, std::memory_order_acq_rel);
    lastWorldUniformBuffer_[context->frameIndex]->uploadToBuffer(hasHistory ? &lastWorldUbo_ : &ubo);

    lastWorldUbo_ = ubo;
}

void Buffers::setAndUploadSkyUniformBuffer(vk::Data::SkyUBO &ubo) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto framework = Renderer::instance().framework();
    auto context = framework->safeAcquireCurrentContext();
    auto vma = framework->vma();
    auto device = framework->device();

    if (skyUniformBuffer_[context->frameIndex] == nullptr) {
        skyUniformBuffer_[context->frameIndex] =
            vk::HostVisibleBuffer::create(vma, device, sizeof(vk::Data::SkyUBO),
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }

    skyUniformBuffer_[context->frameIndex]->uploadToBuffer(&ubo);
}

void Buffers::setAndUploadTextureMappingBuffer(vk::Data::TextureMapping &mapping) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto framework = Renderer::instance().framework();
    auto context = framework->safeAcquireCurrentContext();
    auto vma = framework->vma();
    auto device = framework->device();

    if (textureMappingBuffer_[context->frameIndex] == nullptr) {
        textureMappingBuffer_[context->frameIndex] =
            vk::HostVisibleBuffer::create(vma, device, sizeof(vk::Data::TextureMapping),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }

    textureMappingBuffer_[context->frameIndex]->uploadToBuffer(&mapping);
}

void Buffers::setAndUploadExposureDataBuffer(vk::Data::ExposureData &exposureData) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    auto framework = Renderer::instance().framework();
    auto context = framework->safeAcquireCurrentContext();
    auto vma = framework->vma();
    auto device = framework->device();

    if (exposureDataBuffer_[context->frameIndex] == nullptr) {
        exposureDataBuffer_[context->frameIndex] =
            vk::HostVisibleBuffer::create(vma, device, sizeof(vk::Data::ExposureData),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }

    exposureDataBuffer_[context->frameIndex]->uploadToBuffer(&exposureData);
}

int Buffers::getPostID() {
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();
    return static_cast<int>(overlayPostUniformCount_[context->frameIndex]) - 1;
}

std::shared_ptr<vk::DeviceLocalBuffer> Buffers::getBuffer(uint32_t id) {
    std::lock_guard lock(mtx_);
    if (id >= 0x40000000u) {
        auto found = persistentBuffers_.find(id);
        if (found == persistentBuffers_.end() || !found->second.buffer)
            throw std::runtime_error("Draw requires a live uploaded persistent buffer");
        if (found->second.size > 0 && found->second.uploadReady)
            throw std::runtime_error("Draw requires uploaded persistent buffer data");
        return found->second.buffer;
    }
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();

    auto bufferIter = overlayIndexVertexBuffer_[context->frameIndex].find(id);
    if (!validOverlayIndex_[context->frameIndex].contains(id) ||
        bufferIter == overlayIndexVertexBuffer_[context->frameIndex].end()) {
        throw std::out_of_range("Overlay buffer id is not allocated: " + std::to_string(id));
    }

    return bufferIter->second;
}

std::shared_ptr<vk::HostVisibleBuffer> Buffers::overlayDrawUniformBuffer() {
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();
    return overlayDrawUniformBuffer_[context->frameIndex];
}

uint32_t Buffers::overlayDrawUniformDescriptorRange() {
    return overlayDrawUniformDescriptorRange_;
}

std::shared_ptr<vk::HostVisibleBuffer> Buffers::overlayPostUniformBuffer() {
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();
    return overlayPostUniformBuffer_[context->frameIndex];
}

uint32_t Buffers::overlayPostUniformDescriptorRange() {
    return overlayPostUniformDescriptorRange_;
}

uint32_t Buffers::overlayPostUniformOffset(int postID) {
    if (postID < 0) return 0;
    return static_cast<uint32_t>(postID) * overlayPostUniformStride_;
}

std::shared_ptr<vk::HostVisibleBuffer> Buffers::worldUniformBuffer() {
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();

    if (worldUniformBuffer_[context->frameIndex]) {
        return worldUniformBuffer_[context->frameIndex];
    } else {
        return nullptr;
    }
}

std::shared_ptr<vk::HostVisibleBuffer> Buffers::lastWorldUniformBuffer() {
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();

    if (lastWorldUniformBuffer_[context->frameIndex]) {
        return lastWorldUniformBuffer_[context->frameIndex];
    } else {
        return nullptr;
    }
}

std::shared_ptr<vk::HostVisibleBuffer> Buffers::skyUniformBuffer() {
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();

    if (skyUniformBuffer_[context->frameIndex]) {
        return skyUniformBuffer_[context->frameIndex];
    } else {
        return nullptr;
    }
}

std::shared_ptr<vk::HostVisibleBuffer> Buffers::textureMappingBuffer() {
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();

    if (textureMappingBuffer_[context->frameIndex]) {
        return textureMappingBuffer_[context->frameIndex];
    } else {
        return nullptr;
    }
}

std::shared_ptr<vk::HostVisibleBuffer> Buffers::exposureDataBuffer() {
    auto context = Renderer::instance().framework()->safeAcquireCurrentContext();

    if (exposureDataBuffer_[context->frameIndex]) {
        return exposureDataBuffer_[context->frameIndex];
    } else {
        return nullptr;
    }
}

void Buffers::setUseJitter(bool useJitter) {
    useJitter_ = useJitter;
}
