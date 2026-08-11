#pragma once

#include "core/all_extern.hpp"
#include "core/failure_state.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace vk {
class Instance;
class Window;
class PhysicalDevice;
class PipelineCache;

class Device : public SharedObject<Device> {
  public:
    Device(std::shared_ptr<Instance> instance,
           std::shared_ptr<Window> window,
           std::shared_ptr<PhysicalDevice> physicalDevice);
    ~Device();

    VkDevice &vkDevice();
    VkQueue &mainVkQueue();
    VkQueue &secondaryQueue();

    VkResult createGraphicsPipelines(uint32_t count,
                                     const VkGraphicsPipelineCreateInfo *createInfos,
                                     const VkAllocationCallbacks *allocator,
                                     VkPipeline *pipelines);
    VkResult createComputePipelines(uint32_t count,
                                    const VkComputePipelineCreateInfo *createInfos,
                                    const VkAllocationCallbacks *allocator,
                                    VkPipeline *pipelines);
    VkResult createRayTracingPipelines(VkDeferredOperationKHR deferredOperation,
                                       uint32_t count,
                                       const VkRayTracingPipelineCreateInfoKHR *createInfos,
                                       const VkAllocationCallbacks *allocator,
                                       VkPipeline *pipelines);

    void recordFailure(VkResult result, std::string_view operation) noexcept;
    bool hasFailure() const noexcept;
    bool isDeviceLost() const noexcept;
    VkResult lastFailure() const noexcept;
    std::string lastFailureOperation() const;
    void checkpoint(VkCommandBuffer commands, std::string_view label) const noexcept;
    void nameObject(VkObjectType type, uint64_t handle, const char *label) const noexcept;
    void captureFaultDiagnostics() noexcept;

    bool hasExtendedDynamicState2LogicOp() const;
    bool hasTessellation() const { return tessellation_; }
    bool hasMultiDrawIndirect() const { return multiDrawIndirect_; }
    bool hasDrawIndirectFirstInstance() const { return drawIndirectFirstInstance_; }
    bool isDlssDeviceExtensionsCompatible() const;
    bool isDlssSRDeviceExtensionsCompatible() const { return dlssSRCompatible_; }
    bool isDlssFGDeviceExtensionsCompatible() const { return dlssFGCompatible_; }
    bool isXessDeviceExtensionsCompatible() const;

  private:
    std::shared_ptr<Instance> instance_;
    std::shared_ptr<Window> window_;
    std::shared_ptr<PhysicalDevice> physicalDevice_;

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue mainQueue_ = VK_NULL_HANDLE;
    VkQueue secondaryQueue_ = VK_NULL_HANDLE;
    std::unique_ptr<PipelineCache> pipelineCache_;

    bool extendedDynamicState2LogicOp_ = false;
    bool tessellation_ = false;
    bool multiDrawIndirect_ = false;
    bool drawIndirectFirstInstance_ = false;
    bool dlssDeviceExtensionsCompatible_ = false;
    bool dlssSRCompatible_ = false, dlssFGCompatible_ = false;
    bool xessDeviceExtensionsCompatible_ = false;
    mcvr::failure::State failureState_;
    bool faultDiagnostics_ = false, checkpoints_ = false;
    std::atomic<bool> faultCaptured_{false};

};
}; // namespace vk
