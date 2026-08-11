#pragma once
#include <vulkan/vulkan_core.h>
#include <array>
#include <algorithm>
#include <cstring>

namespace mcvr::diagnostics {
struct FaultCapture {
    VkResult countResult = VK_NOT_READY, detailResult = VK_NOT_READY;
    VkDeviceFaultCountsEXT requested{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkDeviceFaultCountsEXT returned{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    std::array<VkDeviceFaultAddressInfoEXT, 256> addresses{};
    std::array<VkDeviceFaultVendorInfoEXT, 32> vendors{};
    std::array<char, VK_MAX_DESCRIPTION_SIZE> description{};
};

// No allocation, retry, wait or unbounded driver-sized storage on the fatal path.
// A driver may return INCOMPLETE and update counts beyond our capacities; callers clamp iteration.
template<class Query>
FaultCapture captureFault(VkDevice device, Query query) {
    FaultCapture result;
    result.countResult = query(device, &result.requested, nullptr);
    if (result.countResult != VK_SUCCESS) return result;
    result.returned = result.requested;
    result.returned.addressInfoCount = std::min<uint32_t>(result.returned.addressInfoCount, result.addresses.size());
    result.returned.vendorInfoCount = std::min<uint32_t>(result.returned.vendorInfoCount, result.vendors.size());
    result.returned.vendorBinarySize = 0; // deviceFaultVendorBinary was not enabled.
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = result.addresses.data();
    info.pVendorInfos = result.vendors.data();
    result.detailResult = query(device, &result.returned, &info);
    std::memcpy(result.description.data(), info.description, result.description.size());
    result.description.back() = '\0';
    return result;
}
}
