#pragma once

#include <vulkan/vulkan.h>

namespace mcvr {

class FrameAcquireAttempt {
  public:
    void observe(VkResult result) {
        lastResult_ = result;
        acquired_ = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
    }

    [[nodiscard]] bool acquired() const { return acquired_; }
    [[nodiscard]] bool requiresRecreate() const { return lastResult_ == VK_ERROR_OUT_OF_DATE_KHR; }
    [[nodiscard]] bool suboptimal() const { return lastResult_ == VK_SUBOPTIMAL_KHR; }
    [[nodiscard]] VkResult failure() const { return lastResult_; }

    [[nodiscard]] VkResult exhaustedResult() const {
        return acquired_ ? VK_SUCCESS : (requiresRecreate() ? VK_NOT_READY : lastResult_);
    }

  private:
    VkResult lastResult_ = VK_NOT_READY;
    bool acquired_ = false;
};

} // namespace mcvr
