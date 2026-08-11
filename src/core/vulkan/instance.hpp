#pragma once

#include "core/all_extern.hpp"

namespace vk {
class Instance : public SharedObject<Instance> {
  public:
    Instance();
    ~Instance();

    VkInstance &vkInstance();
    bool isDlssInstanceExtensionsCompatible() const;
    bool isDlssSRInstanceExtensionsCompatible() const { return dlssSRCompatible_; }
    bool isDlssFGInstanceExtensionsCompatible() const { return dlssFGCompatible_; }
    bool isXessInstanceExtensionsCompatible() const;

  private:
    VkInstance instance_ = VK_NULL_HANDLE;
    bool dlssInstanceExtensionsCompatible_ = false;
    bool dlssSRCompatible_ = false, dlssFGCompatible_ = false;
    bool xessInstanceExtensionsCompatible_ = false;
    // VkDebugReportCallbackEXT callback_ = VK_NULL_HANDLE;
};
} // namespace vk
