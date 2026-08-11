#pragma once

#include "core/all_extern.hpp"
#include "core/vulkan/pipeline_cache_format.hpp"

#include <filesystem>
#include <mutex>
#include <vector>

namespace vk {

class PipelineCache final {
  public:
    PipelineCache(VkDevice device,
                  const VkPhysicalDeviceProperties &properties,
                  std::filesystem::path cacheRoot);
    ~PipelineCache();

    PipelineCache(const PipelineCache &) = delete;
    PipelineCache &operator=(const PipelineCache &) = delete;

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

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    PipelineCacheIdentity identity_{};
    std::filesystem::path cacheDirectory_;
    mutable std::mutex mutex_;

    struct CreationStats {
        uint64_t calls{};
        uint64_t failedCalls{};
        uint64_t requestedPipelines{};
        uint64_t validFeedbackPipelines{};
        uint64_t applicationCacheHits{};
        uint64_t totalDurationNanoseconds{};
        uint64_t cacheMissDurationNanoseconds{};
    } creationStats_;

    void persist() noexcept;
    std::vector<std::byte> loadLatest() const noexcept;
    void recordCreationFeedback(
        VkResult result,
        uint32_t requestedPipelines,
        std::span<const VkPipelineCreationFeedbackCreateInfo *const> feedbackInfos) noexcept;
};

} // namespace vk
