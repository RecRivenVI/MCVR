#define VMA_IMPLEMENTATION

#include "core/logging.hpp"
#include "core/failure_state.hpp"
#include "core/vulkan/vma.hpp"

#include "core/vulkan/device.hpp"
#include "core/vulkan/instance.hpp"
#include "core/vulkan/physical_device.hpp"

#include <iostream>

auto vmaTableCout() {
    return mcvr::log::info("VMA");
}

auto vmaTableCerr() {
    return mcvr::log::error("VMA");
}

vk::VMA::VMA(std::shared_ptr<Instance> instance,
             std::shared_ptr<PhysicalDevice> physicalDevice,
             std::shared_ptr<Device> device) {
    VmaAllocatorCreateInfo allocatorCreateInfo{};
    allocatorCreateInfo.physicalDevice = physicalDevice->vkPhysicalDevice();
    allocatorCreateInfo.device = device->vkDevice();
    allocatorCreateInfo.instance = instance->vkInstance();
    allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_4;
    allocatorCreateInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    if (const auto result = vmaImportVulkanFunctionsFromVolk(&allocatorCreateInfo, &vulkanFunctions_);
        result != VK_SUCCESS) {
        vmaTableCerr() << "failed to create vulkan function from volk" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::initialization, result,
                             "vmaImportVulkanFunctionsFromVolk");
    } else {
#ifdef DEBUG
        vmaTableCout() << "created vulkan function from volk" << std::endl;
#endif
    }
    allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions_;

    if (const auto result = vmaCreateAllocator(&allocatorCreateInfo, &allocator_); result != VK_SUCCESS) {
        vmaTableCerr() << "failed to create VMA" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::initialization, result, "vmaCreateAllocator");
    } else {
#ifdef DEBUG
        vmaTableCout() << "created VMA" << std::endl;
#endif
    }
}

vk::VMA::~VMA() {
#ifdef DEBUG
    vmaTableCout() << "VMA deconstructed" << std::endl;
#endif
    if (allocator_ != VK_NULL_HANDLE) { vmaDestroyAllocator(allocator_); }
}

VmaAllocator &vk::VMA::allocator() {
    return allocator_;
}
