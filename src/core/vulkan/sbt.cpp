#include "core/logging.hpp"
#include "core/failure_state.hpp"
#include "core/vulkan/sbt.hpp"

#include "core/vulkan/buffer.hpp"
#include "core/vulkan/command.hpp"
#include "core/vulkan/device.hpp"
#include "core/vulkan/physical_device.hpp"
#include "core/vulkan/pipeline.hpp"
#include "core/vulkan/vma.hpp"

#include <cstring>
#include <vector>

static uint32_t align(uint32_t addr, uint32_t alignment) {
    return (addr + alignment - 1) & ~(alignment - 1);
}

vk::SBT::SBT(std::shared_ptr<PhysicalDevice> physicalDevice,
             std::shared_ptr<Device> device,
             std::shared_ptr<VMA> vma,
             std::shared_ptr<RayTracingPipeline> pipeline,
             uint32_t missCount,
             uint32_t hitCount)
    : vma_(vma), device_(device), missCount_(missCount) {
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProperties = physicalDevice->rayTracingProperties();

    handleSize_ = rayTracingProperties.shaderGroupHandleSize;
    uint32_t handleAlignment = rayTracingProperties.shaderGroupHandleAlignment;
    baseAlignment_ = rayTracingProperties.shaderGroupBaseAlignment;
    #ifdef DEBUG
    mcvr::log::info("Sbt") << "handleSize: " << handleSize_ << " handleAlignment: " << handleAlignment
              << " baseAlignment: " << baseAlignment_ << std::endl;
              #endif
    uint32_t groupCount = 1 + missCount + hitCount; // RayGen(1) + Miss + HitGroup

    alignedHandleSize_ = handleSize_;
    if (handleAlignment > 0) { alignedHandleSize_ = align(handleSize_, handleAlignment); }

    shaderHandleStorage_.resize(groupCount * handleSize_);
    vkGetRayTracingShaderGroupHandlesKHR(device->vkDevice(), pipeline->vkPipeline(), 0, groupCount,
                                         shaderHandleStorage_.size(), shaderHandleStorage_.data());

    rgenSBT_ = DeviceLocalBuffer::create(
        vma, device, false, handleSize_,
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 0,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, baseAlignment_);
    rmissSBT_ = DeviceLocalBuffer::create(
        vma, device, false, alignedHandleSize_ * missCount,
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 0,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, baseAlignment_);

    rgenSBT_->uploadToStagingBuffer(&shaderHandleStorage_[0]);
    rmissSBT_->uploadToStagingBuffer(&shaderHandleStorage_[handleSize_]);

    // 设置SBT区域
    raygenRegion_.deviceAddress = rgenSBT_->bufferAddress();
    raygenRegion_.stride = alignedHandleSize_;
    raygenRegion_.size = alignedHandleSize_; // RayGen usually 1 group

    missRegion_.deviceAddress = rmissSBT_->bufferAddress(); // 紧接RayGen后
    missRegion_.stride = alignedHandleSize_;
    missRegion_.size = alignedHandleSize_ * missCount_;
}

vk::SBT::~SBT() {}

void vk::SBT::uploadStaticSBT(std::shared_ptr<CommandBuffer> commandBuffer) {
    auto addBarrier = [commandBuffer](std::shared_ptr<vk::DeviceLocalBuffer> buffer) {
        commandBuffer->barriersBufferImage(
            {vk::CommandBuffer::BufferMemoryBarrier{
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = buffer,
            }},
            {});
    };

    rgenSBT_->uploadToBuffer(commandBuffer);
    addBarrier(rgenSBT_);
    rmissSBT_->uploadToBuffer(commandBuffer);
    addBarrier(rmissSBT_);
}

void vk::SBT::setupHitSBT(std::vector<uint32_t> &hitGroupIndices, std::shared_ptr<CommandBuffer> commandBuffer) {
    VkDeviceSize rhitSBTSize = hitGroupIndices.size() * alignedHandleSize_;
    if (rhitSBTSize == 0) {
        mcvr::log::error("Sbt") << "Hit group should contains something!" << std::endl;
        mcvr::failure::invariant("SBT::setupHitSBT", "hit group list is empty");
    }

    std::vector<uint8_t> cachedRhitSBT(rhitSBTSize, 0);
    for (uint32_t i = 0; i < hitGroupIndices.size(); ++i) {
        uint32_t hit_group_id = hitGroupIndices[i];

        // Access source storage using handleSize_ (tightly packed)
        uint32_t palette_offset = (1 + missCount_ + hit_group_id) * handleSize_;
        uint8_t *pSourceHandle = shaderHandleStorage_.data() + palette_offset;

        // Write to destination SBT using alignedHandleSize_ (stride requirement)
        uint8_t *pDest = cachedRhitSBT.data() + i * alignedHandleSize_;
        memcpy(pDest, pSourceHandle, handleSize_);
    }

    if (rhitSBT_ == nullptr || rhitSBT_->size() != rhitSBTSize) {
        rhitSBT_ = DeviceLocalBuffer::create(
            vma_, device_, false, rhitSBTSize,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 0,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, baseAlignment_);
    }

    rhitSBT_->uploadToStagingBuffer(cachedRhitSBT.data());
    rhitSBT_->uploadToBuffer(commandBuffer);
    commandBuffer->barriersBufferImage(
        {vk::CommandBuffer::BufferMemoryBarrier{
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = rhitSBT_,
        }},
        {});

    hitRegion_.deviceAddress = rhitSBT_->bufferAddress();
    hitRegion_.stride = alignedHandleSize_;
    hitRegion_.size = rhitSBTSize;
}

VkStridedDeviceAddressRegionKHR &vk::SBT::raygenRegion() {
    return raygenRegion_;
}

VkStridedDeviceAddressRegionKHR &vk::SBT::missRegion() {
    return missRegion_;
}

VkStridedDeviceAddressRegionKHR &vk::SBT::hitRegion() {
    return hitRegion_;
}

VkStridedDeviceAddressRegionKHR &vk::SBT::callableRegion() {
    return callableRegion_;
}
