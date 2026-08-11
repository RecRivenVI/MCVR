#include "core/render/chunk_trace.hpp"
#include "core/render/streamline_runtime.hpp"
#include "core/render/frame_acquire_policy.hpp"

#include "core/logging.hpp"
#include "core/render/modules/world/dlss/dlss_frame_generation.hpp"
#include "core/render/render_framework.hpp"

#include "common/shared.hpp"
#include "core/diagnostics/alloc_trace.hpp"
#include "core/diagnostics/fg_timing.hpp"
#include "core/diagnostics/draw_state_trace.hpp"
#include "core/diagnostics/device_loss_trace.hpp"
#include "core/render/buffers.hpp"
#include "core/render/chunks.hpp"
#include "core/render/entities.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/renderer.hpp"
#include "core/render/presentation_rates.hpp"
#include "core/render/textures.hpp"
#include "core/render/world.hpp"
#include "core/render/scene_scope.hpp"
#include "core/failure_state.hpp"

#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>

auto renderFrameworkCout() {
    return mcvr::log::info("Render Framework");
}

auto renderFrameworkCerr() {
    return mcvr::log::error("Render Framework");
}

FrameworkContext::FrameworkContext(std::shared_ptr<Framework> framework, uint32_t frameIndex)
    : framework(framework),
      frameIndex(frameIndex),
      instance(framework->instance_),
      window(framework->window_),
      physicalDevice(framework->physicalDevice_),
      device(framework->device_),
      vma(framework->vma_),
      swapchain(framework->swapchain_),
      swapchainImage(framework->swapchain_->swapchainImages()[frameIndex]),
      commandPool(framework->mainCommandPool_),
      commandProcessedSemaphore(framework->commandProcessedSemaphores_[frameIndex]),
      commandFinishedFence(framework->commandFinishedFences_[frameIndex]),
      uploadCommandBuffer(framework->uploadCommandBuffers_[frameIndex]),
      overlayCommandBuffer(framework->overlayCommandBuffers_[frameIndex]),
      worldCommandBuffer(framework->worldCommandBuffers_[frameIndex]),
      fuseCommandBuffer(framework->fuseCommandBuffers_[frameIndex]) {
    if (framework->timestampValidBits_ != 0) {
        VkQueryPoolCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        createInfo.queryCount = 2;
        const VkResult result = vkCreateQueryPool(device->vkDevice(), &createInfo, nullptr, &frameTimestampQueryPool);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("vkCreateQueryPool(frame timestamps) failed with VkResult=" +
                                     std::to_string(result));
        }
    }
}

std::shared_ptr<vk::CommandBuffer> FrameworkContext::uiPtCommandStorage() {
    return uiPtCommands.storage([&] { return vk::CommandBuffer::create(device, commandPool); });
}

std::shared_ptr<vk::CommandBuffer> FrameworkContext::beginUiPtCommands() {
    return uiPtCommands.begin([&] { return vk::CommandBuffer::create(device, commandPool); });
}

FrameworkContext::~FrameworkContext() {
    if (frameTimestampQueryPool != VK_NULL_HANDLE && device != nullptr) {
        vkDestroyQueryPool(device->vkDevice(), frameTimestampQueryPool, nullptr);
        frameTimestampQueryPool = VK_NULL_HANDLE;
    }
#ifdef DEBUG
    mcvr::log::info("RenderFramework") << "[Context] context deconstructed" << std::endl;
#endif
}

void FrameworkContext::fuseFinal() {
    auto f = framework.lock();

    if (f == nullptr || !f->isRunning()) return;

    auto mainQueueIndex = physicalDevice->mainQueueIndex();
    auto pipelineContext = f->pipeline_->acquirePipelineContext(shared_from_this());
    if (!f->frameGeneration_) f->frameGeneration_ = std::make_unique<DlssFrameGeneration>();
    auto finalColor = f->frameGeneration_->record(*f, *this, pipelineContext);
    mcvr::failure::throwIfFatal();

    fuseCommandBuffer->barriersBufferImage(
        {}, {
                {
                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .oldLayout = finalColor->imageLayout(),
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .srcQueueFamilyIndex = mainQueueIndex,
                    .dstQueueFamilyIndex = mainQueueIndex,
                    .image = finalColor,
                    .subresourceRange = vk::wholeColorSubresourceRange,
                },
                {
                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .oldLayout = swapchainImage->imageLayout(),
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = mainQueueIndex,
                    .dstQueueFamilyIndex = mainQueueIndex,
                    .image = swapchainImage,
                    .subresourceRange = vk::wholeColorSubresourceRange,
                },
            });

    finalColor->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    swapchainImage->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    // TODO: add to command buffer
    VkImageBlit imageBlit{};
    imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlit.srcSubresource.mipLevel = 0;
    imageBlit.srcSubresource.baseArrayLayer = 0;
    imageBlit.srcSubresource.layerCount = 1;
    imageBlit.srcOffsets[0] = {0, 0, 0};
    imageBlit.srcOffsets[1] = {static_cast<int>(finalColor->width()),
                               static_cast<int>(finalColor->height()), 1};
    imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlit.dstSubresource.mipLevel = 0;
    imageBlit.dstSubresource.baseArrayLayer = 0;
    imageBlit.dstSubresource.layerCount = 1;
    imageBlit.dstOffsets[0] = {0, 0, 0};
    imageBlit.dstOffsets[1] = {static_cast<int>(swapchainImage->width()), static_cast<int>(swapchainImage->height()),
                               1};

    vkCmdBlitImage(fuseCommandBuffer->vkCommandBuffer(),
                   finalColor->vkImage(),
                   finalColor->imageLayout(), swapchainImage->vkImage(),
                   swapchainImage->imageLayout(), 1, &imageBlit, VK_FILTER_LINEAR);

    fuseCommandBuffer->barriersBufferImage(
        {}, {{
                 .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                 .oldLayout = finalColor->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = finalColor,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                 .oldLayout = swapchainImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = swapchainImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             }});

    finalColor->imageLayout() = VK_IMAGE_LAYOUT_GENERAL;
    swapchainImage->imageLayout() = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

Framework::Framework() {}

void Framework::init(GLFWwindow *window) {
    instance_ = vk::Instance::create();
    window_ = vk::Window::create(instance_, window);
    physicalDevice_ = vk::PhysicalDevice::create(instance_, window_);
    device_ = vk::Device::create(instance_, window_, physicalDevice_);
    vma_ = vk::VMA::create(instance_, physicalDevice_, device_);
    swapchain_ = vk::Swapchain::create(physicalDevice_, device_, window_);
    mainCommandPool_ = vk::CommandPool::create(physicalDevice_, device_);
    asyncCommandPool_ = vk::CommandPool::create(physicalDevice_, device_, physicalDevice_->secondaryQueueIndex());
    frameResourceRetainer_ = FrameResourceRetainer::create(shared_from_this());

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_->vkPhysicalDevice(), &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_->vkPhysicalDevice(), &queueFamilyCount,
                                             queueFamilies.data());
    const uint32_t mainQueueIndex = physicalDevice_->mainQueueIndex();
    if (mainQueueIndex < queueFamilies.size()) {
        timestampValidBits_ = queueFamilies[mainQueueIndex].timestampValidBits;
        timestampPeriodNs_ = physicalDevice_->properties().limits.timestampPeriod;
    }

    uint32_t imageCount = swapchain_->imageCount();

    // create command buffer for each context
    for (int i = 0; i < imageCount; i++) {
        uploadCommandBuffers_.emplace_back(vk::CommandBuffer::create(device_, mainCommandPool_));
        overlayCommandBuffers_.emplace_back(vk::CommandBuffer::create(device_, mainCommandPool_));
        worldCommandBuffers_.emplace_back(vk::CommandBuffer::create(device_, mainCommandPool_));
        fuseCommandBuffers_.emplace_back(vk::CommandBuffer::create(device_, mainCommandPool_));
    }
    worldAsyncCommandBuffer_ = vk::CommandBuffer::create(device_, asyncCommandPool_);

    for (int i = 0; i < imageCount; i++) { commandFinishedFences_.push_back(vk::Fence::create(device_, true)); }

    for (int i = 0; i < imageCount; i++) { commandProcessedSemaphores_.push_back(vk::Semaphore::create(device_)); }

    for (int i = 0; i < imageCount; i++) { contexts_.push_back(FrameworkContext::create(shared_from_this(), i)); }

    pipeline_ = Pipeline::create(shared_from_this());
}

Framework::~Framework() {
#ifdef DEBUG
    mcvr::log::info("RenderFramework") << "[Framework] framework deconstructed" << std::endl;
#endif
}

VkResult Framework::acquireContext() {
    mcvr::diagnostics::device_loss::note("frame-acquire-begin");
    if (!running_.load(std::memory_order_acquire)) { return inactiveResult(); }
    if (device_ == nullptr || device_->hasFailure()) {
        running_.store(false, std::memory_order_release);
        return inactiveResult();
    }
    if (auto drawable = waitForDrawableWindow(); drawable != VK_SUCCESS) return drawable;

    std::shared_ptr<FrameworkContext> lastContext;
    if (currentContext_) lastContext = currentContext_;

    std::shared_ptr<vk::Semaphore> imageAcquiredSemaphore;
    uint32_t imageIndex = 0;
    VkResult result = VK_SUCCESS;
    mcvr::FrameAcquireAttempt acquireAttempt;
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
        imageAcquiredSemaphore = acquireSemaphore();
        const auto acquireStart = mcvr::fgdiag::start();
        result = vkAcquireNextImageKHR(device_->vkDevice(), swapchain_->vkSwapchain(), UINT64_MAX,
                                       imageAcquiredSemaphore->vkSemaphore(), VK_NULL_HANDLE, &imageIndex);
        acquireAttempt.observe(result);
        mcvr::fgdiag::elapsed(mcvr::fgdiag::MainAcquire, acquireStart);
        if (!acquireAttempt.requiresRecreate()) { break; }

        recycleSemaphore(imageAcquiredSemaphore);
        imageAcquiredSemaphore = nullptr;
        const VkResult recreateResult = recreate(true);
        if (recreateResult != VK_SUCCESS) { return recreateResult; }
    }
    if (!acquireAttempt.acquired()) {
        if (imageAcquiredSemaphore != nullptr) { recycleSemaphore(imageAcquiredSemaphore); }
        currentContext_ = nullptr;
        if (acquireAttempt.requiresRecreate()) { return acquireAttempt.exhaustedResult(); }
        mcvr::log::error("RenderFramework") << "Cannot acquire images from swapchain" << std::endl;
        return recordFailure(acquireAttempt.failure(), "vkAcquireNextImageKHR");
    }
    if (acquireAttempt.suboptimal()) { suboptimalSwapchain_ = true; }

    std::shared_ptr<vk::Fence> fence = contexts_[imageIndex]->commandFinishedFence;
    const auto fenceStart = mcvr::fgdiag::start();
    result = vkWaitForFences(device_->vkDevice(), 1, &fence->vkFence(), true, UINT64_MAX);
    mcvr::diagnostics::device_loss::note("frame-fence-wait", result, imageIndex);
    mcvr::fgdiag::elapsed(mcvr::fgdiag::MainFence, fenceStart);
    if (result != VK_SUCCESS) {
        mcvr::log::info("RenderFramework") << "vkWaitForFences failed with error: " << std::dec << result << std::endl;
        currentContext_ = nullptr;
        return recordFailure(result, "vkWaitForFences(frame)");
    }
    if (contexts_[imageIndex]->frameSubmitted)
        mcvr::chunkTrace::note("frame-complete",-1,-1,contexts_[imageIndex]->chunkTraceSerial);
    result = completeGpuProfile(contexts_[imageIndex]);
    if (result != VK_SUCCESS) { return result; }
    currentContextIndex_ = imageIndex;
    currentContext_ = contexts_[imageIndex];
    currentContext_->worldRenderRequired = false;
    currentContext_->worldRendered = false;
    currentContext_->fgHudlessCaptured = false;
    currentContext_->fgBackgroundDependentDraws = 0;
    currentContext_->fgAffineBackgroundDraws = 0;
    currentContext_->fgWeightedBlurPasses = 0;
    currentContext_->frameSubmitted = false;
    currentContext_->uiPtCommands.newFrame();
    ++currentContext_->uiPtRecordingGeneration;
    if (mcvr::chunkTrace::enabled) currentContext_->chunkTraceSerial=++mcvr::chunkTrace::serial;
    indexHistory_.push(imageIndex);
    if (indexHistory_.size() > swapchain_->imageCount()) indexHistory_.pop();
    frameResourceRetainer_->beginFrame(imageIndex);
    mcvr::diagnostics::recordFrame(
        "acquire", imageIndex, currentContext_->frameSubmitted,
        mcvr::diagnostics::handleValue(currentContext_->commandFinishedFence->vkFence()),
        mcvr::diagnostics::handleValue(currentContext_->overlayCommandBuffer->vkCommandBuffer()));

    if (currentContext_->imageAcquiredSemaphore != VK_NULL_HANDLE) {
        recycleSemaphore(currentContext_->imageAcquiredSemaphore);
        currentContext_->imageAcquiredSemaphore = VK_NULL_HANDLE;
    }
    currentContext_->imageAcquiredSemaphore = imageAcquiredSemaphore;

    currentContext_->uploadCommandBuffer->begin();
    currentContext_->worldCommandBuffer->begin();
    currentContext_->overlayCommandBuffer->begin();
    currentContext_->fuseCommandBuffer->begin();
    if (currentContext_->frameTimestampQueryPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(currentContext_->uploadCommandBuffer->vkCommandBuffer(),
                            currentContext_->frameTimestampQueryPool, 0, 2);
        vkCmdWriteTimestamp(currentContext_->uploadCommandBuffer->vkCommandBuffer(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            currentContext_->frameTimestampQueryPool, 0);
    }

    auto pipelineContext = pipeline_->acquirePipelineContext(currentContext_);
    std::shared_ptr<UIModuleContext> lastUIContext =
        lastContext == nullptr ? nullptr : pipeline_->acquirePipelineContext(lastContext)->uiModuleContext;

    pipelineContext->uiModuleContext->begin(lastUIContext);
    Renderer::instance().buffers()->resetFrame();
    Renderer::instance().textures()->resetFrame();
    Renderer::instance().world()->resetFrame();
    Renderer::instance().world()->chunks()->resetFrame();
    Renderer::instance().world()->entities()->resetFrame();
    static int frames = 0;
    static auto lastTime = std::chrono::high_resolution_clock::now();

    frames++;
    auto currentTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = currentTime - lastTime;

    if (elapsed.count() >= 1.0) {
        std::stringstream ss;
        ss << "FPS: " << frames;

        // GLFW_SetWindowTitle(window_->window(), ss.str().c_str());

        frames = 0;
        lastTime = currentTime;
    }
    return VK_SUCCESS;
}

VkResult Framework::submitCommand() {
    mcvr::diagnostics::device_loss::note("frame-submit-begin");
    if (!running_.load(std::memory_order_acquire)) { return inactiveResult(); }

    if (Renderer::instance().framework()->safeAcquireCurrentContext() == nullptr) { return inactiveResult(); }

    mcvr::failure::runCheckedStage([&] { Renderer::instance().textures()->performQueuedUpload(); });
    mcvr::failure::runCheckedStage([&] { Renderer::instance().buffers()->performQueuedUpload(); });
    mcvr::failure::runCheckedStage([&] { Renderer::instance().buffers()->buildAndUploadOverlayUniformBuffer(); });

    auto pipelineContext = pipeline_->acquirePipelineContext(currentContext_);
    if (Renderer::instance().world()->shouldRender() && !currentContext_->worldRendered) {
        worldDrawStarted_.store(true, std::memory_order_release);
        mcvr::failure::runCheckedStage([&] { pipelineContext->worldPipelineContext->render(); });
        currentContext_->worldRendered = true;
    }
    pipelineContext->uiModuleContext->end();

    currentContext_->fuseFinal();

    if (currentContext_->frameTimestampQueryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(currentContext_->fuseCommandBuffer->vkCommandBuffer(),
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, currentContext_->frameTimestampQueryPool, 1);
    }

    currentContext_->uploadCommandBuffer->end();
    currentContext_->worldCommandBuffer->end();
    currentContext_->uiPtCommands.end();
    currentContext_->overlayCommandBuffer->end();
    currentContext_->fuseCommandBuffer->end();

    std::vector<VkSemaphore> waitSemaphores = {currentContext_->imageAcquiredSemaphore->vkSemaphore()};
    std::vector<VkPipelineStageFlags> waitStageMasks = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    std::vector<VkSemaphore> signalSemaphores = {currentContext_->commandProcessedSemaphore->vkSemaphore()};
    std::vector<VkCommandBuffer> commandbuffers = {
        currentContext_->uploadCommandBuffer->vkCommandBuffer(),
        currentContext_->worldCommandBuffer->vkCommandBuffer(),
        currentContext_->overlayCommandBuffer->vkCommandBuffer(),
        currentContext_->fuseCommandBuffer->vkCommandBuffer(),
    };

    if (currentContext_->uiPtCommands.active())
        commandbuffers.insert(commandbuffers.begin() + 2, currentContext_->uiPtCommands.buffer()->vkCommandBuffer());

    VkSubmitInfo vkSubmitInfo = {};
    vkSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    vkSubmitInfo.waitSemaphoreCount = waitSemaphores.size();
    vkSubmitInfo.pWaitSemaphores = waitSemaphores.data();
    vkSubmitInfo.pWaitDstStageMask = waitStageMasks.data();
    vkSubmitInfo.commandBufferCount = commandbuffers.size();
    vkSubmitInfo.pCommandBuffers = commandbuffers.data();
    vkSubmitInfo.signalSemaphoreCount = signalSemaphores.size();
    vkSubmitInfo.pSignalSemaphores = signalSemaphores.data();

    std::shared_ptr<vk::Fence> fence = currentContext_->commandFinishedFence;
    mcvr::failure::throwIfFatal();
    VkResult result = vkResetFences(device_->vkDevice(), 1, &fence->vkFence());
    if (result != VK_SUCCESS) { return recordFailure(result, "vkResetFences(frame)"); }
    const auto submitStart = mcvr::fgdiag::start();
    result = vkQueueSubmit(device_->mainVkQueue(), 1, &vkSubmitInfo, fence->vkFence());
    mcvr::diagnostics::device_loss::note("frame-queue-submit", result,
        currentContext_ == nullptr ? 0 : currentContext_->frameIndex);
    mcvr::fgdiag::elapsed(mcvr::fgdiag::MainSubmit, submitStart);
    if (result != VK_SUCCESS) { return recordFailure(result, "vkQueueSubmit(frame)"); }
    currentContext_->frameSubmitted = true;
    if (auto world = Renderer::instance().world(); world && currentContext_->worldRendered)
        world->entities()->commitCachedCloudBuild();
    mcvr::chunkTrace::note("frame-submit",-1,-1,currentContext_->chunkTraceSerial);
    currentContext_->timestampQuerySubmitted = currentContext_->frameTimestampQueryPool != VK_NULL_HANDLE;
    mcvr::diagnostics::recordFrame(
        "submit", currentContext_->frameIndex, currentContext_->frameSubmitted,
        mcvr::diagnostics::handleValue(fence->vkFence()),
        mcvr::diagnostics::handleValue(currentContext_->overlayCommandBuffer->vkCommandBuffer()));
    return VK_SUCCESS;
}

VkResult Framework::flushForReadback() {
    std::lock_guard lock(recreateMtx_);
    if (!isRunning()) return inactiveResult();
    auto context = safeAcquireCurrentContext();
    if (!context) return VK_ERROR_INITIALIZATION_FAILED;
    if (context->frameSubmitted) {
        const auto fence = context->commandFinishedFence->vkFence();
        const auto result = vkWaitForFences(device_->vkDevice(), 1, &fence, VK_TRUE, UINT64_MAX);
        return result == VK_SUCCESS ? result : recordFailure(result, "vkWaitForFences(read submitted frame)");
    }
    mcvr::failure::runCheckedStage([&] { Renderer::instance().textures()->performQueuedUpload(); });
    mcvr::failure::runCheckedStage([&] { Renderer::instance().buffers()->performQueuedUpload(); });
    mcvr::failure::runCheckedStage([&] { Renderer::instance().buffers()->buildAndUploadOverlayUniformBuffer(); });
    auto pipelineContext = pipeline_->acquirePipelineContext(context);
    pipelineContext->uiModuleContext->end();
    // A mid-world offscreen read must not render a partial TLAS. fuseWorld marks
    // when the complete world output is a dependency of the recorded overlay work.
    if (context->worldRenderRequired && !context->worldRendered && Renderer::instance().world()->shouldRender()) {
        worldDrawStarted_.store(true, std::memory_order_release);
        mcvr::failure::runCheckedStage([&] { pipelineContext->worldPipelineContext->render(); });
        context->worldRendered = true;
    }
    if (pipeline_->uiModule()->uiPtCollecting)
        throw std::runtime_error("Readback during incomplete UI PT scene collection is not supported");
    std::vector<VkCommandBuffer> commands{context->uploadCommandBuffer->vkCommandBuffer(),
        context->worldCommandBuffer->vkCommandBuffer(), context->overlayCommandBuffer->vkCommandBuffer()};
    if (context->uiPtCommands.active())
        commands.insert(commands.begin() + 2, context->uiPtCommands.buffer()->vkCommandBuffer());
    for (auto command : commands) {
        const auto result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) return recordFailure(result, "vkEndCommandBuffer(readback flush)");
    }
    auto fence = vk::Fence::create(device_);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = commands.size();
    submit.pCommandBuffers = commands.data();
    // These commands touch compositor/FBO images, not the acquired swapchain image.
    // Leave the acquire and present semaphores for the final frame submission.
    mcvr::failure::throwIfFatal();
    auto result = vkQueueSubmit(device_->mainVkQueue(), 1, &submit, fence->vkFence());
    if (result != VK_SUCCESS) return recordFailure(result, "vkQueueSubmit(readback flush)");
    if (auto world = Renderer::instance().world(); world && context->worldRendered)
        world->entities()->commitCachedCloudBuild();
    result = vkWaitForFences(device_->vkDevice(), 1, &fence->vkFence(), VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) return recordFailure(result, "vkWaitForFences(readback flush)");
    for (auto command : commands) {
        result = vkResetCommandBuffer(command, 0);
        if (result != VK_SUCCESS) return recordFailure(result, "vkResetCommandBuffer(readback continuation)");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS) return recordFailure(result, "vkBeginCommandBuffer(readback continuation)");
    }
    return VK_SUCCESS;
}

// Routine surface/rebuild messages use the ordinary logger, not a diagnostic file.
static void radianceDiag(const std::string &line) {
    mcvr::log::debug("RenderFramework") << line << std::endl;
}

// Temporary performance diagnostics: total device memory held by VMA allocations.
static uint64_t vmaAllocatedBytes(const std::shared_ptr<vk::VMA> &vma) {
    if (vma == nullptr) return 0;
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma->allocator(), budgets);
    uint64_t total = 0;
    for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; ++i) total += budgets[i].statistics.allocationBytes;
    return total;
}

static uint64_t vmaAllocationCount(const std::shared_ptr<vk::VMA> &vma) {
    if (vma == nullptr) return 0;
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma->allocator(), budgets);
    uint64_t total = 0;
    for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; ++i) total += budgets[i].statistics.allocationCount;
    return total;
}

void Framework::blurFrameGenerationHudless(FrameworkContext &frame, const vk::Data::OverlayPostUBO &parameters,
                                          const std::shared_ptr<vk::DeviceLocalImage> &color) {
    if (frameGeneration_ && frame.fgHudlessCaptured)
        frameGeneration_->blurHudless(*this, frame, parameters, color);
}

std::shared_ptr<vk::DeviceLocalImage> Framework::frameGenerationHudless(FrameworkContext &frame) const {
    return frameGeneration_ && frame.fgHudlessCaptured ? frameGeneration_->hudless(frame.frameIndex) : nullptr;
}

void Framework::captureFrameGenerationHudless(FrameworkContext &frame, const std::shared_ptr<vk::DeviceLocalImage> &color) {
    if (!Renderer::options.dlssFrameGeneration) return;
    if (!frameGeneration_) frameGeneration_ = std::make_unique<DlssFrameGeneration>();
    frameGeneration_->captureHudless(*this, frame, color);
}

VkResult Framework::present() {
    mcvr::diagnostics::device_loss::note("frame-present-begin");
    if (!running_.load(std::memory_order_acquire)) { return inactiveResult(); }
    if (currentContext_ == nullptr) { return recordFailure(VK_ERROR_INITIALIZATION_FAILED, "present(no context)"); }

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &currentContext_->commandProcessedSemaphore->vkSemaphore();

    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_->vkSwapchain();
    presentInfo.pImageIndices = &currentContext_->frameIndex;

    const auto presentStart = mcvr::fgdiag::start();
    mcvr::StreamlineRuntime::get().marker(sl::PCLMarker::ePresentStart);
    VkResult result = vkQueuePresentKHR(device_->mainVkQueue(), &presentInfo);
    mcvr::diagnostics::device_loss::note("frame-queue-present", result, currentContext_->frameIndex);
    mcvr::fgdiag::elapsed(mcvr::fgdiag::FirstPresent, presentStart);
    const bool firstAccepted = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
    auto &sl=mcvr::StreamlineRuntime::get();
    sl.marker(sl::PCLMarker::ePresentEnd);
    const auto outputCount=sl.presentedFrames();
    mcvr::presentationRates.recordCounts(firstAccepted, Renderer::options.dlssFrameGeneration ? outputCount : uint32_t(firstAccepted));
    if (auto observer = mcvr::audit::sink.load(std::memory_order_acquire);
        observer && (observer->flags & MCVR_AUDIT_ALLOCATIONS) && observer->wantsFrame()) {
        McvrAuditFrame frame{vmaAllocatedBytes(vma()), vmaAllocationCount(vma()),
            swapchain_->vkExtent().width, swapchain_->vkExtent().height, swapchain_->imageCount(),
            static_cast<int32_t>(result)};
        observer->frame(&frame);
    }

    const auto now = std::chrono::steady_clock::now();
    if (result == VK_SUBOPTIMAL_KHR) suboptimalSwapchain_ = true;
    bool checkSurface = vk::Window::framebufferResized || Renderer::options.presentationChanged ||
        (suboptimalSwapchain_ && now - lastSurfaceCheck_ >= std::chrono::milliseconds(250));
    if (result == VK_ERROR_OUT_OF_DATE_KHR || checkSurface ||
        Renderer::options.needRecreate || pipeline_->isRecreationNeeded) {
        return recreate(result == VK_ERROR_OUT_OF_DATE_KHR, checkSurface);
    } else if (result == VK_SUBOPTIMAL_KHR) {
        // Still presentable; repeated SUBOPTIMAL reports do not trigger full rebuilds.
        limitFrameRate();
        return VK_SUCCESS;
    } else if (result != VK_SUCCESS) {
        mcvr::log::error("RenderFramework") << "failed to submit present command buffer" << std::endl;
        return recordFailure(result, "vkQueuePresentKHR");
    }

    limitFrameRate();
    return VK_SUCCESS;
}

uint32_t Framework::effectiveFrameRateLimit() const {
    const uint32_t maxFps = Renderer::options.maxFps;

    if (maxFps == 0 || maxFps >= 260) {
        return 0;
    }

    return maxFps;
}

void Framework::limitFrameRate() {
    if (mcvr::StreamlineRuntime::get().pacesFrames()) { frameLimitAnchor_ = {}; return; }
    mcvr::fgdiag::Scope timing(mcvr::fgdiag::Limiter);
    const uint32_t fpsLimit = effectiveFrameRateLimit();
    if (fpsLimit == 0) {
        frameLimitAnchor_ = {};
        frameLimitFps_ = 0;
        return;
    }

    const auto frameDuration =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / fpsLimit));
    if (frameDuration <= std::chrono::steady_clock::duration::zero()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (frameLimitFps_ != fpsLimit || frameLimitAnchor_ == std::chrono::steady_clock::time_point{}) {
        frameLimitAnchor_ = now;
        frameLimitFps_ = fpsLimit;
    }

    const auto target = frameLimitAnchor_ + frameDuration;
    if (now < target) {
        std::this_thread::sleep_until(target);
        frameLimitAnchor_ = std::chrono::steady_clock::now();
    } else {
        frameLimitAnchor_ = now;
    }
}

VkResult Framework::waitForDrawableWindow() {
    static bool drawableMinimized = false;
    int width = 0, height = 0;
    GLFW_GetFramebufferSize(window_->window(), &width, &height);
    if ((width <= 0 || height <= 0) && !drawableMinimized) {
        drawableMinimized = true;
        radianceDiag("[radiance-surface] drawable minimized (0x0)");
    }
    while (width <= 0 || height <= 0) {
        if (nonBlockingResize || GLFW_WindowShouldClose(window_->window()) || !isRunning()) return VK_NOT_READY;
        // The main thread pumps events without retaining the recreation mutex or discarding resources.
        GLFW_WaitEventsTimeout(0.05);
        GLFW_GetFramebufferSize(window_->window(), &width, &height);
    }
    if (drawableMinimized) {
        drawableMinimized = false;
        radianceDiag("[radiance-surface] drawable restored " + std::to_string(width) + "x" + std::to_string(height));
    }
    return VK_SUCCESS;
}

VkResult Framework::recreate(bool forcePresentation, bool checkSurface) {
    if (!running_.load(std::memory_order_acquire)) { return inactiveResult(); }

    if (auto drawable = waitForDrawableWindow(); drawable != VK_SUCCESS) return drawable;

    std::unique_lock<std::recursive_mutex> lck(Renderer::instance().framework()->recreateMtx());
    const bool reportNativeProgress = Pipeline::nativeRebuildActive();
    const auto surfaceCheckTime = std::chrono::steady_clock::now();

    try {
        const bool surfaceStateHandled = checkSurface || forcePresentation;
        const bool presentation = forcePresentation || (checkSurface && swapchain_->needsReconstruction());
        const bool worldChanged = Renderer::options.needRecreate || pipeline_->isRecreationNeeded;
        radianceDiag("[radiance-recreate] enter forcePresentation=" + std::to_string(forcePresentation) +
                     " checkSurface=" + std::to_string(checkSurface) +
                     " presentation=" + std::to_string(presentation) +
                     " worldChanged=" + std::to_string(worldChanged) +
                     " framebufferResized=" + std::to_string(vk::Window::framebufferResized) +
                     " presentationChanged=" + std::to_string(Renderer::options.presentationChanged) +
                     " suboptimal=" + std::to_string(suboptimalSwapchain_));
        if (checkSurface || forcePresentation) lastSurfaceCheck_ = surfaceCheckTime;
        if (!presentation && !worldChanged) {
            if (surfaceStateHandled) {
                vk::Window::framebufferResized = false;
                Renderer::options.presentationChanged = false;
                suboptimalSwapchain_ = false;
            }
            radianceDiag("[radiance-recreate] noop (surface state only)");
            return VK_SUCCESS;
        }

        // Flush SL's asynchronous present worker before destroying tagged inputs.
        const VkResult idleResult = mcvr::StreamlineRuntime::get().ready() ? waitDeviceIdle() : waitRenderQueueIdle();
        if (idleResult != VK_SUCCESS) {
            if (reportNativeProgress) { Pipeline::endNativeRebuild(); }
            return idleResult;
        }
        const VkResult backendIdle = waitBackendQueueIdle();
        if (backendIdle != VK_SUCCESS) return backendIdle;

        for (const auto &context : contexts_) {
            const VkResult profileResult = completeGpuProfile(context);
            if (profileResult != VK_SUCCESS) {
                if (reportNativeProgress) { Pipeline::endNativeRebuild(); }
                return profileResult;
            }
        }
        frameGeneration_.reset();
        if (presentation && !mcvr::optionalFeatureRequestSatisfied(
                mcvr::StreamlineRuntime::get().setFGLoaded(Renderer::options.dlssFrameGeneration))) {
            mcvr::log::error("RenderFramework") << "Cannot apply the requested DLSS-G plugin state" << std::endl;
            if (reportNativeProgress) { Pipeline::endNativeRebuild(); }
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto oldExtent = swapchain_->vkExtent();
        const uint32_t oldCount = swapchain_->imageCount();
        if (presentation) {
            // No command is pending now. Drop image views before destroying their swapchain.
            for (auto &context : contexts_) context->swapchainImage.reset();
            swapchain_->reconstruct();
        }
        const uint32_t size = swapchain_->imageCount();
        const auto extent = swapchain_->vkExtent();
        const bool resizeTargets = oldCount != size || oldExtent.width != extent.width || oldExtent.height != extent.height;
        if (oldCount != size) {
            currentContext_ = nullptr;
            currentContextIndex_ = 0;
            contexts_.clear();
            uploadCommandBuffers_.clear(); overlayCommandBuffers_.clear();
            worldCommandBuffers_.clear(); fuseCommandBuffers_.clear();
            commandFinishedFences_.clear(); commandProcessedSemaphores_.clear();
            indexHistory_ = {};
            frameResourceRetainer_->resetFrameCount(size);
            for (uint32_t i = 0; i < size; ++i) {
                uploadCommandBuffers_.push_back(vk::CommandBuffer::create(device_, mainCommandPool_));
                overlayCommandBuffers_.push_back(vk::CommandBuffer::create(device_, mainCommandPool_));
                worldCommandBuffers_.push_back(vk::CommandBuffer::create(device_, mainCommandPool_));
                fuseCommandBuffers_.push_back(vk::CommandBuffer::create(device_, mainCommandPool_));
                commandFinishedFences_.push_back(vk::Fence::create(device_, true));
                commandProcessedSemaphores_.push_back(vk::Semaphore::create(device_));
            }
            for (uint32_t i = 0; i < size; ++i) contexts_.push_back(FrameworkContext::create(shared_from_this(), i));
        } else if (presentation) {
            for (uint32_t i = 0; i < size; ++i) contexts_[i]->swapchainImage = swapchain_->swapchainImages()[i];
        }
        if (resizeTargets || worldChanged) {
            pipeline_->recreate(shared_from_this(), resizeTargets, worldChanged);
            Renderer::instance().buffers()->invalidateWorldHistory();
            Renderer::instance().textures()->bindAllTextures();
        }
        {
            radianceDiag("[radiance-recreate] rebuild images=" + std::to_string(size) +
                         " extent=" + std::to_string(extent.width) + "x" + std::to_string(extent.height) +
                         " oldImages=" + std::to_string(oldCount) +
                         " oldExtent=" + std::to_string(oldExtent.width) + "x" + std::to_string(oldExtent.height) +
                         " resizeTargets=" + std::to_string(resizeTargets) +
                         " worldChanged=" + std::to_string(worldChanged) +
                         " vram_mb=" + std::to_string(vmaAllocatedBytes(vma()) >> 20));
            if (pipeline_ && pipeline_->uiModule()) {
                radianceDiag("[radiance-ui] recreate " + pipeline_->uiModule()->resourceDiagnostics());
            }
        }
        Renderer::options.needRecreate = false;
        pipeline_->isRecreationNeeded = false;
        if (surfaceStateHandled) {
            Renderer::options.presentationChanged = false;
            vk::Window::framebufferResized = false;
            suboptimalSwapchain_ = false;
        }
        if (reportNativeProgress) {
            Pipeline::endNativeRebuild();
        }
        return VK_SUCCESS;
    } catch (const mcvr::failure::FatalError &failure) {
        renderFrameworkCerr() << "swapchain recreation failed: " << failure.what() << std::endl;
        if (reportNativeProgress) {
            Pipeline::endNativeRebuild();
        }
        return recordFailure(failure.result(), failure.operation().c_str());
    } catch (const std::exception &exception) {
        renderFrameworkCerr() << "swapchain recreation failed: " << exception.what() << std::endl;
        if (reportNativeProgress) {
            Pipeline::endNativeRebuild();
        }
        return recordFailure(VK_ERROR_INITIALIZATION_FAILED, "Framework::recreate");
    } catch (...) {
        renderFrameworkCerr() << "swapchain recreation failed with an unknown exception" << std::endl;
        if (reportNativeProgress) { Pipeline::endNativeRebuild(); }
        return recordFailure(VK_ERROR_UNKNOWN, "Framework::recreate");
    }
}

VkResult Framework::warmupCurrentPipeline() {
    if (!running_.load(std::memory_order_acquire)) {
        return inactiveResult();
    }
    if (pipeline_ == nullptr || pipeline_->worldPipelineBlueprint() == nullptr ||
        (!Renderer::options.needRecreate && !pipeline_->isRecreationNeeded)) {
        return VK_SUCCESS;
    }
    if (currentContext_ == nullptr) {
        return VK_NOT_READY;
    }
    auto world = Renderer::instance().world();
    if (world == nullptr || world->shouldRender() ||
        worldDrawStarted_.load(std::memory_order_acquire)) {
        return VK_NOT_READY;
    }

    return recreate(false, false);
}

VkResult Framework::waitDeviceIdle() {
    if (device_ == nullptr) { return VK_SUCCESS; }
    if (device_->isDeviceLost()) { return VK_ERROR_DEVICE_LOST; }
    const VkResult result = vkDeviceWaitIdle(device_->vkDevice());
    if (result != VK_SUCCESS) { return recordFailure(result, "vkDeviceWaitIdle"); }
    return VK_SUCCESS;
}

VkResult Framework::waitRenderQueueIdle() {
    if (device_ == nullptr) { return VK_SUCCESS; }
    if (device_->isDeviceLost()) { return VK_ERROR_DEVICE_LOST; }
    const VkResult result = vkQueueWaitIdle(device_->mainVkQueue());
    if (result != VK_SUCCESS) { return recordFailure(result, "vkQueueWaitIdle(main)"); }
    return VK_SUCCESS;
}

VkResult Framework::waitBackendQueueIdle() {
    if (device_ == nullptr) { return VK_SUCCESS; }
    if (device_->isDeviceLost()) { return VK_ERROR_DEVICE_LOST; }
    const VkResult result = vkQueueWaitIdle(device_->secondaryQueue());
    if (result != VK_SUCCESS) { return recordFailure(result, "vkQueueWaitIdle(secondary)"); }
    return VK_SUCCESS;
}

void Framework::close() {
    running_.store(false, std::memory_order_release);
    if (!closeGate_.begin()) return;
    if (!isDeviceLost()) waitDeviceIdle();
    frameGeneration_.reset();
    if (pipeline_ != nullptr) { pipeline_->close(); }
}

bool Framework::isRunning() const {
    return running_.load(std::memory_order_acquire);
}

bool Framework::isDeviceLost() const noexcept {
    return device_ == nullptr ? mcvr::failure::isDeviceLost() : device_->isDeviceLost();
}

VkResult Framework::lastFailure() const noexcept {
    return device_ == nullptr ? mcvr::failure::globalState.firstResult() : device_->lastFailure();
}

std::string Framework::lastFailureDescription() const {
    if (device_ == nullptr || !device_->hasFailure()) { return {}; }
    return device_->lastFailureOperation() + " failed with VkResult=" + std::to_string(device_->lastFailure());
}

VkResult Framework::recordFailure(VkResult result, const char *operation) noexcept {
    if (result == VK_SUCCESS) { return result; }
    if (device_ != nullptr) {
        device_->recordFailure(result, operation);
    } else {
        mcvr::failure::record(result == VK_ERROR_DEVICE_LOST ? mcvr::failure::Kind::deviceLost
                                                             : mcvr::failure::Kind::runtime,
                              result, operation);
    }
    running_.store(false, std::memory_order_release);
    if (result == VK_ERROR_DEVICE_LOST) {
        mcvr::diagnostics::device_loss::dump(operation, result);
    }
    try {
        renderFrameworkCerr() << operation << " failed with VkResult=" << result
                              << (result == VK_ERROR_DEVICE_LOST ? " (device lost)" : "") << std::endl;
    } catch (...) {
        mcvr::failure::globalState.noteDetailFailure();
    }
    return result;
}

VkResult Framework::inactiveResult() const {
    const VkResult failure = lastFailure();
    return failure == VK_SUCCESS ? VK_NOT_READY : failure;
}

uint32_t Framework::beginGpuProfile() {
    std::lock_guard<std::mutex> lock(gpuProfileMtx_);
    uint32_t sequence = nextGpuProfileSequence_++;
    if (nextGpuProfileSequence_ == 0 || nextGpuProfileSequence_ > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        nextGpuProfileSequence_ = 1;
    }

    if (currentContext_ == nullptr || currentContext_->frameTimestampQueryPool == VK_NULL_HANDLE ||
        currentContext_->gpuProfileSequence != 0) {
        completedGpuProfiles_[sequence] = 0;
        while (completedGpuProfiles_.size() > 64) { completedGpuProfiles_.erase(completedGpuProfiles_.begin()); }
    } else {
        currentContext_->gpuProfileSequence = sequence;
    }
    return sequence;
}

bool Framework::isGpuProfileReady(uint32_t sequence) {
    std::lock_guard<std::mutex> lock(gpuProfileMtx_);
    return completedGpuProfiles_.contains(sequence);
}

uint64_t Framework::gpuProfileTimeNs(uint32_t sequence) {
    std::lock_guard<std::mutex> lock(gpuProfileMtx_);
    const auto result = completedGpuProfiles_.find(sequence);
    return result == completedGpuProfiles_.end() ? 0 : result->second;
}

VkResult Framework::completeGpuProfile(const std::shared_ptr<FrameworkContext> &context) {
    if (context == nullptr || !context->timestampQuerySubmitted || context->frameTimestampQueryPool == VK_NULL_HANDLE) {
        return VK_SUCCESS;
    }

    uint64_t timestamps[2]{};
    const VkResult result = vkGetQueryPoolResults(device_->vkDevice(), context->frameTimestampQueryPool, 0, 2,
                                                   sizeof(timestamps), timestamps, sizeof(uint64_t),
                                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (result != VK_SUCCESS) { return recordFailure(result, "vkGetQueryPoolResults(frame timestamps)"); }

    context->timestampQuerySubmitted = false;
    const uint32_t sequence = context->gpuProfileSequence;
    context->gpuProfileSequence = 0;
    const uint64_t mask = timestampValidBits_ >= 64 ? std::numeric_limits<uint64_t>::max()
                                                    : ((uint64_t{1} << timestampValidBits_) - 1);
    const uint64_t elapsedTicks = (timestamps[1] - timestamps[0]) & mask;
    const long double elapsedNs = static_cast<long double>(elapsedTicks) * timestampPeriodNs_;
    const uint64_t durationNs = elapsedNs >= static_cast<long double>(std::numeric_limits<uint64_t>::max())
                                    ? std::numeric_limits<uint64_t>::max()
                                    : static_cast<uint64_t>(elapsedNs);
    mcvr::fgdiag::add(mcvr::fgdiag::GpuFrame, double(durationNs)/1e6);
    if (sequence == 0) { return VK_SUCCESS; }
    storeGpuProfileResult(sequence, durationNs);
    return VK_SUCCESS;
}

void Framework::storeGpuProfileResult(uint32_t sequence, uint64_t durationNs) {
    std::lock_guard<std::mutex> lock(gpuProfileMtx_);
    completedGpuProfiles_[sequence] = durationNs;
    while (completedGpuProfiles_.size() > 64) { completedGpuProfiles_.erase(completedGpuProfiles_.begin()); }
}

VkResult Framework::takeScreenshot(bool withUI, int width, int height, int channel, void *dstPointer) {
    if (!isRunning()) { return inactiveResult(); }
    if (indexHistory_.empty()) { return VK_NOT_READY; }

    uint32_t targetIndex = indexHistory_.front();
    auto context = contexts_[targetIndex];
    std::shared_ptr<vk::Fence> fence = context->commandFinishedFence;
    VkResult result = vkWaitForFences(device_->vkDevice(), 1, &fence->vkFence(), true, UINT64_MAX);
    if (result != VK_SUCCESS) {
        mcvr::log::info("RenderFramework") << "vkWaitForFences failed with error for screenshot: " << std::dec << result << std::endl;
        return recordFailure(result, "vkWaitForFences(screenshot source)");
    }

    std::shared_ptr<vk::HostVisibleBuffer> dstBuffer;
    std::shared_ptr<vk::DeviceLocalImage> srcImage;

    if (withUI) {
        auto pipelineContext = pipeline_->acquirePipelineContext(context);
        srcImage = pipelineContext->uiModuleContext->overlayDrawColorImage;

        uint32_t finalImageBufferSize =
            srcImage->width() * srcImage->height() * srcImage->layer() * vk::formatToByte(srcImage->vkFormat());
        if (finalImageBufferSize != width * height * channel) { return VK_ERROR_FORMAT_NOT_SUPPORTED; }

        dstBuffer =
            vk::HostVisibleBuffer::create(vma_, device_, finalImageBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    } else {
        auto pipelineContext = pipeline_->acquirePipelineContext(context);
        srcImage = pipelineContext->worldPipelineContext->outputImage;

        uint32_t worldImageBufferSize =
            srcImage->width() * srcImage->height() * srcImage->layer() * vk::formatToByte(srcImage->vkFormat());
        if (worldImageBufferSize != width * height * channel) { return VK_ERROR_FORMAT_NOT_SUPPORTED; }

        dstBuffer =
            vk::HostVisibleBuffer::create(vma_, device_, worldImageBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }

    VkImageLayout initialLayout = srcImage->imageLayout();
    auto mainQueueIndex = physicalDevice_->mainQueueIndex();

    std::shared_ptr<vk::CommandBuffer> oneTimeBuffer = vk::CommandBuffer::create(device_, mainCommandPool_);
    oneTimeBuffer->begin();

    oneTimeBuffer->barriersBufferImage(
        {},
        {{
            .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout = initialLayout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .image = srcImage,
            .subresourceRange = vk::wholeColorSubresourceRange,
        }});
    VkBufferImageCopy bufferImageCopy{};
    bufferImageCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bufferImageCopy.imageSubresource.mipLevel = 0;
    bufferImageCopy.imageSubresource.baseArrayLayer = 0;
    bufferImageCopy.imageSubresource.layerCount = 1;
    bufferImageCopy.imageExtent.width = srcImage->width();
    bufferImageCopy.imageExtent.height = srcImage->height();
    bufferImageCopy.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(oneTimeBuffer->vkCommandBuffer(), srcImage->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dstBuffer->vkBuffer(), 1, &bufferImageCopy);
    oneTimeBuffer
        ->barriersBufferImage(
            {}, {{
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .newLayout = initialLayout,
                    .srcQueueFamilyIndex = mainQueueIndex,
                    .dstQueueFamilyIndex = mainQueueIndex,
                    .image = srcImage,
                    .subresourceRange = vk::wholeColorSubresourceRange,
                }})
        ->end();

    VkSubmitInfo vkSubmitInfo = {};
    vkSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    vkSubmitInfo.waitSemaphoreCount = 0;
    vkSubmitInfo.commandBufferCount = 1;
    vkSubmitInfo.pCommandBuffers = &oneTimeBuffer->vkCommandBuffer();
    vkSubmitInfo.signalSemaphoreCount = 0;
    std::shared_ptr<vk::Fence> oneTimeFence = vk::Fence::create(device_);

    result = vkQueueSubmit(device_->mainVkQueue(), 1, &vkSubmitInfo, oneTimeFence->vkFence());
    if (result != VK_SUCCESS) { return recordFailure(result, "vkQueueSubmit(screenshot)"); }
    result = vkWaitForFences(device_->vkDevice(), 1, &oneTimeFence->vkFence(), true, UINT64_MAX);
    if (result != VK_SUCCESS) {
        mcvr::log::info("RenderFramework") << "vkWaitForFences failed with error for screenshot: " << std::dec << result << std::endl;
        return recordFailure(result, "vkWaitForFences(screenshot copy)");
    }

    dstBuffer->downloadFromBuffer();
    std::memcpy(dstPointer, dstBuffer->mappedPtr(), dstBuffer->size());
    return VK_SUCCESS;
}

std::recursive_mutex &Framework::recreateMtx() {
    return recreateMtx_;
}

std::shared_ptr<vk::Instance> Framework::instance() {
    return instance_;
}

std::shared_ptr<vk::Window> Framework::window() {
    return window_;
}

std::shared_ptr<vk::PhysicalDevice> Framework::physicalDevice() {
    return physicalDevice_;
}

std::shared_ptr<vk::Device> Framework::device() {
    return device_;
}

std::shared_ptr<vk::VMA> Framework::vma() {
    return vma_;
}

std::shared_ptr<vk::Swapchain> Framework::swapchain() {
    return swapchain_;
}

std::shared_ptr<vk::CommandPool> Framework::mainCommandPool() {
    return mainCommandPool_;
}

std::shared_ptr<vk::CommandPool> Framework::asyncCommandPool() {
    return asyncCommandPool_;
}

std::shared_ptr<vk::CommandBuffer> Framework::worldAsyncCommandBuffer() {
    return worldAsyncCommandBuffer_;
}

std::vector<std::shared_ptr<vk::Semaphore>> &Framework::commandProcessedSemaphores() {
    return commandProcessedSemaphores_;
}

std::vector<std::shared_ptr<vk::Fence>> &Framework::commandFinishedFences() {
    return commandFinishedFences_;
}

uint32_t Framework::recordingContextCount() {
    if (auto scope = SceneRecordingScope::active()) return static_cast<uint32_t>(scope->contexts.size());
    return swapchain_->imageCount();
}

std::vector<std::shared_ptr<FrameworkContext>> &Framework::contexts() {
    if (auto scene = SceneRecordingScope::active()) return scene->contexts;
    return contexts_;
}

std::shared_ptr<FrameworkContext> Framework::safeAcquireCurrentContext() {
    if (auto scene = SceneRecordingScope::active()) return scene->current;
    std::unique_lock<std::recursive_mutex> lck(recreateMtx_);
    // for continous window operation, currentContext_ will always be reset, busy waiting
    while (currentContext_ == nullptr && isRunning()) {
        // ensure currentContext_ is not nullptr after seapchain recreation
        if (acquireContext() != VK_SUCCESS) { break; }
    }
    return currentContext_;
}

std::shared_ptr<Pipeline> Framework::pipeline() {
    return pipeline_;
}

FrameResourceRetainer &Framework::frameResourceRetainer() {
    return *frameResourceRetainer_;
}

std::shared_ptr<vk::Semaphore> Framework::acquireSemaphore() {
    std::shared_ptr<vk::Semaphore> semaphore;
    if (recycledImageAcquiredSemaphores_.empty()) {
        semaphore = vk::Semaphore::create(device_);
    } else {
        semaphore = recycledImageAcquiredSemaphores_.front();
        recycledImageAcquiredSemaphores_.pop();
    }
    return semaphore;
}

void Framework::recycleSemaphore(std::shared_ptr<vk::Semaphore> semaphore) {
    recycledImageAcquiredSemaphores_.push(semaphore);
}

FrameResourceRetainer::FrameResourceRetainer(std::shared_ptr<Framework> framework) {
    retainedResourcesByFrame_.resize(framework->swapchain_->imageCount());
}

void FrameResourceRetainer::beginFrame(uint32_t frameIndex) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);

    currentFrameIndex_ = frameIndex;
    retainedResourcesByFrame_[currentFrameIndex_].clear();
}

void FrameResourceRetainer::resetFrameCount(uint32_t frameCount) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);
    retainedResourcesByFrame_.clear();
    retainedResourcesByFrame_.resize(frameCount);
    currentFrameIndex_ = 0;
}
