#pragma once

#include <vulkan/vulkan.h>
#include <algorithm>
#include <utility>

namespace vk::detail {
// The caller must have drained all GPU users and must re-record commands before reuse.
// A fresh set removes stale Vulkan descriptors as well as their CPU resource references.
// On allocation failure keep ownership until fatal cleanup; the freed set cannot be used.
template <class Resources, class Free, class Allocate>
VkResult resetIdleDescriptorSet(uint32_t index, VkDescriptorSet &set, Resources &resources,
                                Free &&free, Allocate &&allocate) {
    if (set != VK_NULL_HANDLE) {
        const VkResult result = std::forward<Free>(free)(set);
        if (result != VK_SUCCESS) return result;
        set = VK_NULL_HANDLE;
    }
    VkDescriptorSet replacement = VK_NULL_HANDLE;
    const VkResult result = std::forward<Allocate>(allocate)(replacement);
    if (result != VK_SUCCESS) return result;
    set = replacement;
    for (auto entry = resources.begin(); entry != resources.end();) {
        if (entry->first.set == index) entry = resources.erase(entry);
        else ++entry;
    }
    return VK_SUCCESS;
}
} // namespace vk::detail
