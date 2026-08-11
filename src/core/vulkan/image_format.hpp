#pragma once

#include <vulkan/vulkan_core.h>

namespace vk {
inline VkImageAspectFlags formatAspects(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_UNDEFINED: return 0;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

// Layout tracking is per image: packed depth/stencil aspects transition together.
// Sampling views and copy regions may still select an individual aspect.
inline VkImageSubresourceRange formatSubresourceRange(VkFormat format, uint32_t mipLevels,
                                                     uint32_t depth, uint32_t layers) noexcept {
    return {formatAspects(format), 0, mipLevels, 0, depth > 1 ? 1u : layers};
}
} // namespace vk
