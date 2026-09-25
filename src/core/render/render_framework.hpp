#pragma once

#include "core/util/close_gate.hpp"
#include "core/diagnostics/gpu_profile.hpp"
#include "core/util/deferred_frame_commands.hpp"

#include "core/logging.hpp"
#include "common/shared.hpp"
#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/render/modules/world/dlss/dlss_wrapper.hpp"
#include "core/render/pipeline.hpp"
#include "core/vulkan/all_core_vulkan.hpp"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

class DlssFrameGeneration;
class Framework;
class UIModule;
struct UIModuleContext;

class FrameResourceRetainer : public SharedObject<FrameResourceRetainer> {
  public:
    FrameResourceRetainer(std::shared_ptr<Framework> framework);

    template <typename T>
    void retain(std::shared_ptr<T> resource);

    void beginFrame(uint32_t frameIndex);
    void resetFrameCount(uint32_t frameCount);

  private:
    std::vector<std::vector<std::shared_ptr<void>>> retainedResourcesByFrame_;
    uint32_t currentFrameIndex_ = 0;
    std::recursive_mutex mtx_;
};

struct FrameworkContext : public SharedObject<FrameworkContext> {
    std::weak_ptr<Framework> framework;

    uint32_t frameIndex;
    uint64_t chunkTraceSerial = 0;
    uint64_t uiPtRecordingGeneration = 0;

    std::shared_ptr<vk::Instance> instance;
    std::shared_ptr<vk::Window> window;
    std::shared_ptr<vk::PhysicalDevice> physicalDevice;
    std::shared_ptr<vk::Device> device;
    std::shared_ptr<vk::VMA> vma;
    std::shared_ptr<vk::Swapchain> swapchain;
    std::shared_ptr<vk::SwapchainImage> swapchainImage;
    std::shared_ptr<vk::CommandPool> commandPool;
    std::shared_ptr<vk::Semaphore> imageAcquiredSemaphore = nullptr;
    std::shared_ptr<vk::Semaphore> commandProcessedSemaphore;
    std::shared_ptr<vk::Fence> commandFinishedFence;

    std::shared_ptr<vk::CommandBuffer> uploadCommandBuffer;
    std::shared_ptr<vk::CommandBuffer> overlayCommandBuffer;
    std::shared_ptr<vk::CommandBuffer> worldCommandBuffer;
    mcvr::DeferredFrameCommands<vk::CommandBuffer> uiPtCommands;
    std::shared_ptr<vk::CommandBuffer> fuseCommandBuffer;

    mcvr::profile::GpuFrame auditGpu;
    int auditUpload=-1, auditWorld=-1, auditOverlay=-1, auditFuse=-1;
    VkQueryPool frameTimestampQueryPool = VK_NULL_HANDLE;
    uint32_t gpuProfileSequence = 0;
    bool timestampQuerySubmitted = false;
    bool worldRenderRequired = false;
    bool worldRendered = false;
    bool fgHudlessCaptured = false;
    uint32_t fgBackgroundDependentDraws = 0;
    uint32_t fgAffineBackgroundDraws = 0;
    uint32_t fgWeightedBlurPasses = 0;
    bool frameSubmitted = false;

    FrameworkContext(std::shared_ptr<Framework> framework, uint32_t frame_index);
    ~FrameworkContext();

    void fuseFinal();
    std::shared_ptr<vk::CommandBuffer> uiPtCommandStorage();
    std::shared_ptr<vk::CommandBuffer> beginUiPtCommands();
};

class Framework : public SharedObject<Framework> {
    friend FrameworkContext;
    friend FrameResourceRetainer;

  public:
    Framework();
    ~Framework();

    void init(GLFWwindow *window);
    VkResult acquireContext();
    VkResult submitCommand();
    VkResult present();
    void captureFrameGenerationHudless(FrameworkContext &frame, const std::shared_ptr<vk::DeviceLocalImage> &color);
    void blurFrameGenerationHudless(FrameworkContext &frame, const vk::Data::OverlayPostUBO &parameters,
                                   const std::shared_ptr<vk::DeviceLocalImage> &color);
    std::shared_ptr<vk::DeviceLocalImage> frameGenerationHudless(FrameworkContext &frame) const;
    VkResult recreate(bool forcePresentation = false, bool checkSurface = true);
    VkResult warmupCurrentPipeline();
    VkResult waitDeviceIdle();
    VkResult waitRenderQueueIdle();
    VkResult waitBackendQueueIdle();
    VkResult flushForReadback();
    VkResult readPixels(int x, int y, int width, int height, int format, int type, void *destination);
    void close();
    bool isRunning() const;
    bool isDeviceLost() const noexcept;
    // Early rendering runs off the GLFW event thread and must never wait for window events.
    bool nonBlockingResize = false;
    VkResult lastFailure() const noexcept;
    std::string lastFailureDescription() const;
    VkResult recordFailure(VkResult result, const char *operation) noexcept;

    VkResult takeScreenshot(bool withUI, int width, int height, int channel, void *dstPointer);

    std::recursive_mutex &recreateMtx();

    std::shared_ptr<vk::Instance> instance();
    std::shared_ptr<vk::Window> window();
    std::shared_ptr<vk::PhysicalDevice> physicalDevice();
    std::shared_ptr<vk::Device> device();
    std::shared_ptr<vk::VMA> vma();
    std::shared_ptr<vk::Swapchain> swapchain();
    std::shared_ptr<vk::CommandPool> mainCommandPool();
    std::shared_ptr<vk::CommandPool> asyncCommandPool();

    std::shared_ptr<vk::CommandBuffer> worldAsyncCommandBuffer();

    std::vector<std::shared_ptr<vk::Semaphore>> &commandProcessedSemaphores();
    std::vector<std::shared_ptr<vk::Fence>> &commandFinishedFences();
    std::vector<std::shared_ptr<FrameworkContext>> &contexts();
    uint32_t recordingContextCount();
    std::shared_ptr<FrameworkContext> safeAcquireCurrentContext();

    std::shared_ptr<Pipeline> pipeline();

    FrameResourceRetainer &frameResourceRetainer();

    uint32_t beginGpuProfile();
    bool isGpuProfileReady(uint32_t sequence);
    uint64_t gpuProfileTimeNs(uint32_t sequence);
    uint32_t effectiveFrameRateLimit() const;

  private:
    std::shared_ptr<vk::Semaphore> acquireSemaphore();
    void recycleSemaphore(std::shared_ptr<vk::Semaphore> semaphore);
    VkResult waitForDrawableWindow();
    void limitFrameRate();
    VkResult inactiveResult() const;
    VkResult completeGpuProfile(const std::shared_ptr<FrameworkContext> &context);
    void storeGpuProfileResult(uint32_t sequence, uint64_t durationNs);

  private:
    std::shared_ptr<vk::Instance> instance_;
    std::shared_ptr<vk::Window> window_;
    std::shared_ptr<vk::PhysicalDevice> physicalDevice_;
    std::shared_ptr<vk::Device> device_;
    std::shared_ptr<vk::VMA> vma_;
    std::shared_ptr<vk::Swapchain> swapchain_;
    std::shared_ptr<vk::CommandPool> mainCommandPool_;
    std::shared_ptr<vk::CommandPool> asyncCommandPool_;

    std::vector<std::shared_ptr<vk::CommandBuffer>> uploadCommandBuffers_;
    std::vector<std::shared_ptr<vk::CommandBuffer>> overlayCommandBuffers_;
    std::vector<std::shared_ptr<vk::CommandBuffer>> worldCommandBuffers_;
    std::vector<std::shared_ptr<vk::CommandBuffer>> fuseCommandBuffers_;
    std::shared_ptr<vk::CommandBuffer> worldAsyncCommandBuffer_;

    std::unique_ptr<DlssFrameGeneration> frameGeneration_;
    std::shared_ptr<Pipeline> pipeline_;

    std::vector<std::shared_ptr<vk::Semaphore>> commandProcessedSemaphores_;
    std::vector<std::shared_ptr<vk::Fence>> commandFinishedFences_;

    std::vector<std::shared_ptr<FrameworkContext>> contexts_;

    std::shared_ptr<FrameworkContext> currentContext_ = nullptr;
    uint32_t currentContextIndex_ = 0;
    std::queue<uint32_t> indexHistory_;

    std::queue<std::shared_ptr<vk::Semaphore>> recycledImageAcquiredSemaphores_;
    std::recursive_mutex recreateMtx_;
    std::chrono::steady_clock::time_point lastSurfaceCheck_{};
    bool suboptimalSwapchain_ = false;

    std::atomic_bool running_{true};
    mcvr::CloseGate closeGate_;
    std::atomic_bool worldDrawStarted_{false};
    std::chrono::steady_clock::time_point frameLimitAnchor_{};
    uint32_t frameLimitFps_ = 0;

    uint32_t timestampValidBits_ = 0;
    float timestampPeriodNs_ = 0.0F;
    uint32_t nextGpuProfileSequence_ = 1;
    std::map<uint32_t, uint64_t> completedGpuProfiles_;
    std::mutex gpuProfileMtx_;

    std::shared_ptr<FrameResourceRetainer> frameResourceRetainer_;
};

template <typename T>
void FrameResourceRetainer::retain(std::shared_ptr<T> resource) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);

    if (resource != nullptr) {
        retainedResourcesByFrame_[currentFrameIndex_].push_back(resource);

#ifdef DEBUG
        if constexpr (std::is_same_v<T, vk::DeviceLocalImage>) {
            mcvr::log::info("RenderFramework") << "Frame resource retainer enqueued image (" << resource->debugName
                      << ") in frame: " << currentFrameIndex_
                      << std::endl;
        }
#endif
    }
}
