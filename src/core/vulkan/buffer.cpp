#include "core/vulkan/buffer.hpp"

#include "core/logging.hpp"
#include "core/failure_state.hpp"

#include "core/diagnostics/alloc_trace.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/vulkan/command.hpp"
#include "core/vulkan/device.hpp"
#include "core/vulkan/vma.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>

auto bufferCout() {
    return mcvr::log::info("Buffer");
}

auto bufferCerr() {
    return mcvr::log::error("Buffer");
}

namespace {
[[noreturn]] void throwBufferAllocationFailure(VkResult result,
                                                const char *kind,
                                                size_t size) {
    throw std::runtime_error(std::string("Failed to allocate ") + kind + ": VkResult=" + std::to_string(result) +
                             ", size=" + std::to_string(size));
}
} // namespace

vk::HostVisibleBuffer::HostVisibleBuffer(std::shared_ptr<VMA> vma,
                                         std::shared_ptr<Device> device,
                                         size_t size,
                                         VkBufferUsageFlags usage)
    : vma_(vma), device_(device), size_(size), bufferUsage_(usage) {
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size_;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocationInfo = {};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    // if (bufferUsage_ & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    //     allocationInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    // }

    const VkResult createResult =
        vmaCreateBuffer(vma_->allocator(), &bufferInfo, &allocationInfo, &buffer_, &allocation_, &allocationInfo_);
    if (createResult != VK_SUCCESS) {
        throwBufferAllocationFailure(createResult, "host-visible buffer", size_);
    }
    mappedPtr_ = allocationInfo_.pMappedData;
    allocTraceTag_ = mcvr::diag::recordAllocCreate("host", size_);

    if (bufferUsage_ & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo deviceAddressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                                    .buffer = buffer_};
        bufferAddress_ = vkGetBufferDeviceAddress(device_->vkDevice(), &deviceAddressInfo);
    }
}

vk::HostVisibleBuffer::HostVisibleBuffer(std::shared_ptr<VMA> vma,
                                         std::shared_ptr<Device> device,
                                         size_t size,
                                         VkBufferUsageFlags usage,
                                         VkDeviceSize minAlignment)
    : vma_(vma), device_(device), size_(size), bufferUsage_(usage) {
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size_;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocationInfo = {};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    // if (bufferUsage_ & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    //     allocationInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    // }

    const VkResult createResult = vmaCreateBufferWithAlignment(vma_->allocator(), &bufferInfo, &allocationInfo,
                                                                minAlignment, &buffer_, &allocation_,
                                                                &allocationInfo_);
    if (createResult != VK_SUCCESS) {
        throwBufferAllocationFailure(createResult, "aligned host-visible buffer", size_);
    }
    mappedPtr_ = allocationInfo_.pMappedData;
    allocTraceTag_ = mcvr::diag::recordAllocCreate("host", size_);

    if (bufferUsage_ & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo deviceAddressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                                    .buffer = buffer_};
        bufferAddress_ = vkGetBufferDeviceAddress(device_->vkDevice(), &deviceAddressInfo);
    }
}

vk::HostVisibleBuffer::~HostVisibleBuffer() {
    mcvr::diag::recordAllocDestroy("host", size_, allocTraceTag_);
    if (buffer_ != VK_NULL_HANDLE || allocation_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(vma_->allocator(), buffer_, allocation_);
    }

#ifdef DEBUG
// bufferCout() << "host visible buffer deconstructed" << std::endl;
#endif
}

void vk::HostVisibleBuffer::downloadFromBuffer() {
    downloadFromBuffer(size_, 0);
}

void vk::HostVisibleBuffer::downloadFromBuffer(size_t size, size_t offset) {
    vmaInvalidateAllocation(vma_->allocator(), allocation_, offset, size);
}

void vk::HostVisibleBuffer::uploadToBuffer(void *src) {
    uploadToBuffer(src, size_, 0);
}

void vk::HostVisibleBuffer::uploadToBuffer(void *src, size_t size, size_t offset) {
    std::memcpy(static_cast<uint8_t *>(mappedPtr_) + offset, src, size);
    vmaFlushAllocation(vma_->allocator(), allocation_, offset, size);
}

void vk::HostVisibleBuffer::flush() {
    vmaFlushAllocation(vma_->allocator(), allocation_, 0, size_);
}

size_t vk::HostVisibleBuffer::size() {
    return size_;
}

VkBuffer &vk::HostVisibleBuffer::vkBuffer() {
    return buffer_;
}

void *vk::HostVisibleBuffer::mappedPtr() {
    return mappedPtr_;
}

VkDeviceAddress &vk::HostVisibleBuffer::bufferAddress() {
    if (!(bufferUsage_ & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)) {
        bufferCerr() << "VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT not specified when try to get bufferAddress"
                     << std::endl;
        mcvr::failure::invariant("HostVisibleBuffer::bufferAddress",
                                 "buffer lacks VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT");
    }
    return bufferAddress_;
}

vk::TemporaryStagingBuffer::TemporaryStagingBuffer(std::shared_ptr<VMA> vma,
                                                   VkBuffer buffer,
                                                   VmaAllocation allocation,
                                                   size_t size)
    : vma_(vma), buffer_(buffer), allocation_(allocation), size_(size) {
    allocTraceTag_ = mcvr::diag::recordAllocCreate("staging", size_);
}

vk::TemporaryStagingBuffer::~TemporaryStagingBuffer() {
    mcvr::diag::recordAllocDestroy("staging", size_, allocTraceTag_);
    if (buffer_ != VK_NULL_HANDLE || allocation_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(vma_->allocator(), buffer_, allocation_);
    }
}

vk::DeviceLocalBuffer::DeviceLocalBuffer(std::shared_ptr<VMA> vma,
                                         std::shared_ptr<Device> device,
                                         size_t size,
                                         VkBufferUsageFlags usageExceptTransfer)
    : DeviceLocalBuffer(vma, device, true, size, usageExceptTransfer) {}

vk::DeviceLocalBuffer::DeviceLocalBuffer(std::shared_ptr<VMA> vma,
                                         std::shared_ptr<Device> device,
                                         bool persistStaging,
                                         size_t size,
                                         VkBufferUsageFlags usageExceptTransfer)
    : DeviceLocalBuffer(
          vma, device, persistStaging, size, usageExceptTransfer, 0, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE) {}

vk::DeviceLocalBuffer::DeviceLocalBuffer(std::shared_ptr<VMA> vma,
                                         std::shared_ptr<Device> device,
                                         bool persistStaging,
                                         size_t size,
                                         VkBufferUsageFlags usageExceptTransfer,
                                         VmaAllocationCreateFlags vmaAllocationFlags,
                                         VmaMemoryUsage vmaUsage)
    : vma_(vma),
      device_(device),
      persistStaging_(persistStaging),
      size_(size),
      vmaAllocationFlags_(vmaAllocationFlags),
      vmaUsage_(vmaUsage) {
#ifdef DEBUG
// bufferCout() << "created buffer with size: " << size_ << std::endl;
#endif

    if (persistStaging_) {
        // staging buffer
        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size_;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocationInfo = {};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        const VkResult stagingResult = vmaCreateBuffer(vma_->allocator(), &bufferInfo, &allocationInfo,
                                                        &stagingBuffer_, &stagingAllocation_,
                                                        &stagingAllocationInfo_);
        if (stagingResult != VK_SUCCESS) {
            throwBufferAllocationFailure(stagingResult, "persistent staging buffer", size_);
        }
        mappedPtr_ = stagingAllocationInfo_.pMappedData;
    }

    // buffer
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size_;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | usageExceptTransfer;
    bufferUsage_ = bufferInfo.usage;

    VmaAllocationCreateInfo allocationInfo = {};
    allocationInfo.flags = vmaAllocationFlags;
    allocationInfo.usage = vmaUsage;
    // if (usageExceptTransfer & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    //     allocationInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    //     mcvr::log::info("Buffer") << "already specified VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT" << std::endl;
    // }

    const VkResult createResult =
        vmaCreateBuffer(vma_->allocator(), &bufferInfo, &allocationInfo, &buffer_, &allocation_, &allocationInfo_);
    if (createResult != VK_SUCCESS) {
        if (stagingBuffer_ != VK_NULL_HANDLE || stagingAllocation_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(vma_->allocator(), stagingBuffer_, stagingAllocation_);
            stagingBuffer_ = VK_NULL_HANDLE;
            stagingAllocation_ = VK_NULL_HANDLE;
            mappedPtr_ = nullptr;
        }
        throwBufferAllocationFailure(createResult, "device-local buffer", size_);
    }

    if (usageExceptTransfer & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo deviceAddressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                                    .buffer = buffer_};
        bufferAddress_ = vkGetBufferDeviceAddress(device_->vkDevice(), &deviceAddressInfo);
    }
    allocTraceTag_ = mcvr::diag::recordAllocCreate("buffer", size_);
}

vk::DeviceLocalBuffer::DeviceLocalBuffer(std::shared_ptr<VMA> vma,
                                         std::shared_ptr<Device> device,
                                         bool persistStaging,
                                         size_t size,
                                         VkBufferUsageFlags usageExceptTransfer,
                                         VmaAllocationCreateFlags vmaAllocationFlags,
                                         VmaMemoryUsage vmaUsage,
                                         VkDeviceSize minAlignment)
    : vma_(vma),
      device_(device),
      persistStaging_(persistStaging),
      size_(size),
      vmaAllocationFlags_(vmaAllocationFlags),
      vmaUsage_(vmaUsage) {
#ifdef DEBUG
// bufferCout() << "created buffer with size: " << size_ << std::endl;
#endif

    if (persistStaging_) {
        // staging buffer
        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size_;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocationInfo = {};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        const VkResult stagingResult = vmaCreateBuffer(vma_->allocator(), &bufferInfo, &allocationInfo,
                                                        &stagingBuffer_, &stagingAllocation_,
                                                        &stagingAllocationInfo_);
        if (stagingResult != VK_SUCCESS) {
            throwBufferAllocationFailure(stagingResult, "persistent aligned staging buffer", size_);
        }
        mappedPtr_ = stagingAllocationInfo_.pMappedData;
    }

    // buffer
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size_;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | usageExceptTransfer;
    bufferUsage_ = bufferInfo.usage;

    VmaAllocationCreateInfo allocationInfo = {};
    allocationInfo.flags = vmaAllocationFlags;
    allocationInfo.usage = vmaUsage;
    // if (usageExceptTransfer & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    //     allocationInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    //     mcvr::log::info("Buffer") << "already specified VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT" << std::endl;
    // }

    const VkResult createResult = vmaCreateBufferWithAlignment(vma_->allocator(), &bufferInfo, &allocationInfo,
                                                                minAlignment, &buffer_, &allocation_,
                                                                &allocationInfo_);
    if (createResult != VK_SUCCESS) {
        if (stagingBuffer_ != VK_NULL_HANDLE || stagingAllocation_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(vma_->allocator(), stagingBuffer_, stagingAllocation_);
            stagingBuffer_ = VK_NULL_HANDLE;
            stagingAllocation_ = VK_NULL_HANDLE;
            mappedPtr_ = nullptr;
        }
        throwBufferAllocationFailure(createResult, "aligned device-local buffer", size_);
    }

    if (usageExceptTransfer & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo deviceAddressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                                    .buffer = buffer_};
        bufferAddress_ = vkGetBufferDeviceAddress(device_->vkDevice(), &deviceAddressInfo);
    }
    allocTraceTag_ = mcvr::diag::recordAllocCreate("buffer", size_);
}

vk::DeviceLocalBuffer::~DeviceLocalBuffer() {
    mcvr::diag::recordAllocDestroy("buffer", size_, allocTraceTag_);
    if (transientStagingRetainer_ != nullptr) {
        stagingBuffer_ = VK_NULL_HANDLE;
        stagingAllocation_ = VK_NULL_HANDLE;
        mappedPtr_ = nullptr;
        transientStagingRetainer_ = nullptr;
    }
    if (stagingBuffer_ != VK_NULL_HANDLE || stagingAllocation_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(vma_->allocator(), stagingBuffer_, stagingAllocation_);
    }
    if (buffer_ != VK_NULL_HANDLE || allocation_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(vma_->allocator(), buffer_, allocation_);
    }

#ifdef DEBUG
// bufferCout() << "device local buffer deconstructed" << std::endl;
#endif
}

void vk::DeviceLocalBuffer::downloadFromStagingBuffer(void *dest) {
    downloadFromStagingBuffer(dest, size_, 0);
}

void vk::DeviceLocalBuffer::downloadFromStagingBuffer(void *dest, size_t size, size_t offset) {
    if (!persistStaging_) {
        if (stagingBuffer_ != VK_NULL_HANDLE || stagingAllocation_ != VK_NULL_HANDLE || mappedPtr_ != nullptr) {
            bufferCerr() << "if not persist staging, the staging buffer should not exist!" << std::endl;
            mcvr::failure::invariant("DeviceLocalBuffer::downloadFromStagingBuffer",
                                     "non-persistent staging state is already allocated");
        }

        // staging buffer
        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size_;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocationInfo = {};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        const VkResult stagingResult = vmaCreateBuffer(vma_->allocator(), &bufferInfo, &allocationInfo,
                                                        &stagingBuffer_, &stagingAllocation_,
                                                        &stagingAllocationInfo_);
        if (stagingResult != VK_SUCCESS) {
            stagingBuffer_ = VK_NULL_HANDLE;
            stagingAllocation_ = VK_NULL_HANDLE;
            mappedPtr_ = nullptr;
            throwBufferAllocationFailure(stagingResult, "transient download staging buffer", size_);
        }
        mappedPtr_ = stagingAllocationInfo_.pMappedData;
    }

    vmaInvalidateAllocation(vma_->allocator(), stagingAllocation_, offset, size);
    std::memcpy(dest, mappedPtr_, size);

    if (!persistStaging_) {
        vmaDestroyBuffer(vma_->allocator(), stagingBuffer_, stagingAllocation_);
        stagingBuffer_ = VK_NULL_HANDLE;
        stagingAllocation_ = VK_NULL_HANDLE;
        mappedPtr_ = nullptr;
    }
}

void vk::DeviceLocalBuffer::uploadToStagingBuffer(void *src) {
    uploadToStagingBuffer(src, size_, 0);
}

void vk::DeviceLocalBuffer::uploadToStagingBuffer(void *src, size_t size, size_t offset) {
    prepareStagingBuffer();
    std::memcpy(static_cast<uint8_t *>(mappedPtr_) + offset, src, size);
    vmaFlushAllocation(vma_->allocator(), stagingAllocation_, offset, size);
}

void vk::DeviceLocalBuffer::writeToStagingBuffer(const std::function<void(void *, size_t)> &write) {
    prepareStagingBuffer();
    write(mappedPtr_, size_);
    const auto result = vmaFlushAllocation(vma_->allocator(), stagingAllocation_, 0, size_);
    if (result != VK_SUCCESS) {
        device_->recordFailure(result, "flush directly written upload staging");
        mcvr::failure::raise(mcvr::failure::Kind::runtime, result, "flush directly written upload staging");
    }
}

void vk::DeviceLocalBuffer::prepareStagingBuffer() {
    if (!persistStaging_) {
        if (stagingBuffer_ != VK_NULL_HANDLE || stagingAllocation_ != VK_NULL_HANDLE || mappedPtr_ != nullptr) {
            bufferCerr() << "if not persist staging, the staging buffer should not exist!" << std::endl;
            mcvr::failure::invariant("DeviceLocalBuffer::uploadToStagingBuffer",
                                     "non-persistent staging state is already allocated");
        }

        // staging buffer
        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size_;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocationInfo = {};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        const VkResult stagingResult = vmaCreateBuffer(vma_->allocator(), &bufferInfo, &allocationInfo,
                                                        &stagingBuffer_, &stagingAllocation_,
                                                        &stagingAllocationInfo_);
        if (stagingResult != VK_SUCCESS) {
            stagingBuffer_ = VK_NULL_HANDLE;
            stagingAllocation_ = VK_NULL_HANDLE;
            mappedPtr_ = nullptr;
            throwBufferAllocationFailure(stagingResult, "transient upload staging buffer", size_);
        }
        mappedPtr_ = stagingAllocationInfo_.pMappedData;
        transientStagingRetainer_ = TemporaryStagingBuffer::create(vma_, stagingBuffer_, stagingAllocation_, size_);
    }

}

void vk::DeviceLocalBuffer::flushStagingBuffer() {
    if (!persistStaging_) { return; }
    vmaFlushAllocation(vma_->allocator(), stagingAllocation_, 0, size_);
}

void vk::DeviceLocalBuffer::releaseStaging() {
    if (!persistStaging_) { return; }
    if (stagingBuffer_ == VK_NULL_HANDLE && stagingAllocation_ == VK_NULL_HANDLE) { return; }

    vmaDestroyBuffer(vma_->allocator(), stagingBuffer_, stagingAllocation_);
    stagingBuffer_ = VK_NULL_HANDLE;
    stagingAllocation_ = VK_NULL_HANDLE;
    mappedPtr_ = nullptr;
}

void vk::DeviceLocalBuffer::downloadFromBuffer(VkCommandBuffer cmdBuffer) {
    downloadFromBuffer(cmdBuffer, size_, 0, 0);
}

void vk::DeviceLocalBuffer::downloadFromBuffer(VkCommandBuffer cmdBuffer,
                                               size_t size,
                                               size_t srcOffset,
                                               size_t dstOffset) {
    VkBufferCopy copyRegion = {srcOffset, dstOffset, size};
    vkCmdCopyBuffer(cmdBuffer, buffer_, stagingBuffer_, 1, &copyRegion);
}

void vk::DeviceLocalBuffer::uploadToBuffer(VkCommandBuffer cmdBuffer) {
    uploadToBuffer(cmdBuffer, size_, 0, 0);
}

void vk::DeviceLocalBuffer::uploadToBuffer(VkCommandBuffer cmdBuffer, size_t size, size_t srcOffset, size_t dstOffset) {
    if (stagingBuffer_ == VK_NULL_HANDLE || buffer_ == VK_NULL_HANDLE)
        throw std::logic_error("Buffer upload requires an unconsumed staging payload");
    VkBufferCopy copyRegion = {srcOffset, dstOffset, size};
    vkCmdCopyBuffer(cmdBuffer, stagingBuffer_, buffer_, 1, &copyRegion);
}

void vk::DeviceLocalBuffer::uploadToBuffer(std::shared_ptr<CommandBuffer> cmdBuffer) {
    uploadToBuffer(cmdBuffer, size_, 0, 0);
}

void vk::DeviceLocalBuffer::uploadToBuffer(std::shared_ptr<CommandBuffer> cmdBuffer,
                                           size_t size,
                                           size_t srcOffset,
                                           size_t dstOffset) {
    uploadToBuffer(cmdBuffer->vkCommandBuffer(), size, srcOffset, dstOffset);

    if (!persistStaging_ && transientStagingRetainer_ != nullptr) {
        Renderer::instance().framework()->frameResourceRetainer().retain(transientStagingRetainer_);
        transientStagingRetainer_ = nullptr;
        stagingBuffer_ = VK_NULL_HANDLE;
        stagingAllocation_ = VK_NULL_HANDLE;
        mappedPtr_ = nullptr;
    }
}

size_t vk::DeviceLocalBuffer::size() {
    return size_;
}

VkBuffer &vk::DeviceLocalBuffer::vkStagingBuffer() {
    return stagingBuffer_;
}

VkBuffer &vk::DeviceLocalBuffer::vkBuffer() {
    return buffer_;
}

void *vk::DeviceLocalBuffer::mappedPtr() {
    return mappedPtr_;
}

VkDeviceAddress &vk::DeviceLocalBuffer::bufferAddress() {
    if (!(bufferUsage_ & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)) {
        bufferCerr() << "VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT not specified when try to get bufferAddress"
                     << std::endl;
        mcvr::failure::invariant("DeviceLocalBuffer::bufferAddress",
                                 "buffer lacks VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT");
    }
    return bufferAddress_;
}
