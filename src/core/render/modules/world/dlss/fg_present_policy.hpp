#pragma once
#include <vulkan/vulkan_core.h>

namespace mcvr::dlss {
inline void applyAsyncPresentResult(VkResult result, bool &failed, bool &reset, bool &checkSurface) noexcept {
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        reset = true;
        checkSurface = true;
    } else if (result < 0) {
        failed = true;
    }
}
}
