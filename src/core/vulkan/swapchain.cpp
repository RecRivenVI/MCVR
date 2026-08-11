#include "core/render/streamline_runtime.hpp"

#include "core/logging.hpp"
#include "core/failure_state.hpp"
#include "core/vulkan/swapchain.hpp"

#include "core/vulkan/device.hpp"
#include "core/vulkan/image.hpp"
#include "core/vulkan/physical_device.hpp"
#include "core/vulkan/window.hpp"

#include "core/render/renderer.hpp"

#include <algorithm>
#include <iostream>
#include <vector>

auto swapchainCout() {
    return mcvr::log::info("Swapchain");
}

auto swapchainCerr() {
    return mcvr::log::error("Swapchain");
}

vk::Swapchain::Swapchain(std::shared_ptr<PhysicalDevice> physicalDevice,
                         std::shared_ptr<Device> device,
                         std::shared_ptr<Window> window)
    : physicalDevice_(physicalDevice), device_(device), window_(window) {
    reconstruct();
}

VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats) {
    // can choose any format
    if (availableFormats.size() == 1 && availableFormats[0].format == VK_FORMAT_UNDEFINED) {
#ifdef DEBUG
        mcvr::log::info("Swapchain") << "selected surface format: " << VK_FORMAT_R8G8B8A8_UNORM
                  << " color space: " << VK_COLORSPACE_SRGB_NONLINEAR_KHR << std::endl;
#endif
        return {VK_FORMAT_R8G8B8A8_UNORM, VK_COLORSPACE_SRGB_NONLINEAR_KHR};
    }

    auto formatRank = [](VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM: return 0;
            case VK_FORMAT_B8G8R8A8_UNORM: return 1;
            case VK_FORMAT_R8G8B8A8_SRGB: return 2;
            case VK_FORMAT_B8G8R8A8_SRGB: return 3;
            default: return 4;
        }
    };

    auto selectedFormat = std::min_element(
        availableFormats.begin(), availableFormats.end(),
        [&formatRank](const auto &lhs, const auto &rhs) { return formatRank(lhs.format) < formatRank(rhs.format); });

    if (selectedFormat->format == VK_FORMAT_R8G8B8A8_SRGB || selectedFormat->format == VK_FORMAT_B8G8R8A8_SRGB) {
        swapchainCerr() << "warning: selected SRGB surface format (" << selectedFormat->format
                        << "), UNORM format unavailable, this may cause color space error" << std::endl;
    }

#ifdef DEBUG
    mcvr::log::info("Swapchain") << "selected surface format: " << selectedFormat->format << " color space: " << selectedFormat->colorSpace
              << std::endl;
#endif
    return *selectedFormat;
}

VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR &surfaceCapabilities, uint32_t width, uint32_t height) {
    if (surfaceCapabilities.currentExtent.width == -1) {
        VkExtent2D swapChainExtent = {};

        swapChainExtent.width = std::min(std::max(width, surfaceCapabilities.minImageExtent.width),
                                         surfaceCapabilities.maxImageExtent.width);
        swapChainExtent.height = std::min(std::max(height, surfaceCapabilities.minImageExtent.height),
                                          surfaceCapabilities.maxImageExtent.height);
        return swapChainExtent;
    } else {
        return surfaceCapabilities.currentExtent;
    }
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR> presentModes) {
    const bool fg=Renderer::options.dlssFrameGeneration && mcvr::StreamlineRuntime::get().supported(sl::kFeatureDLSS_G);
    if (Renderer::options.vsync && !fg) { return VK_PRESENT_MODE_FIFO_KHR; }

    for (const auto &presentMode : presentModes) {
        if (presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR) { return presentMode; }
    }

    // If mailbox is unavailable, fall back to FIFO (guaranteed to be available)
    return VK_PRESENT_MODE_FIFO_KHR;
}

void vk::Swapchain::reconstruct() {
    // Find surface capabilities
    VkSurfaceCapabilitiesKHR surfaceCapabilities;
    if (const auto result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physicalDevice_->vkPhysicalDevice(), window_->vkSurface(), &surfaceCapabilities);
        result != VK_SUCCESS) {
        swapchainCerr() << "failed to acquire presentation surface capabilities" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime, result,
                             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    }

    maxExtent_ = surfaceCapabilities.maxImageExtent;
    minExtent_ = surfaceCapabilities.minImageExtent;

    // Determine number of images for swap chain
    imageCount_ = surfaceCapabilities.minImageCount + 1;
    imageCount_ = std::clamp(imageCount_, (uint32_t)2, (uint32_t)3);
    if (surfaceCapabilities.maxImageCount != 0 && imageCount_ > surfaceCapabilities.maxImageCount) {
        imageCount_ = surfaceCapabilities.maxImageCount;
    }

#ifdef DEBUG
    swapchainCout() << "using " << imageCount_ << " images for swap chain" << std::endl;
#endif

    // Find supported surface formats
    uint32_t formatCount;
    const auto formatCountResult = vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice_->vkPhysicalDevice(), window_->vkSurface(), &formatCount, nullptr);
    if (formatCountResult != VK_SUCCESS ||
        formatCount == 0) {
        swapchainCerr() << "failed to get number of supported surface formats" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime,
                             formatCountResult == VK_SUCCESS ? VK_ERROR_FORMAT_NOT_SUPPORTED : formatCountResult,
                             "vkGetPhysicalDeviceSurfaceFormatsKHR(count)");
    }

    std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
    if (const auto result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice_->vkPhysicalDevice(), window_->vkSurface(), &formatCount, surfaceFormats.data());
        result != VK_SUCCESS) {
        swapchainCerr() << "failed to get supported surface formats" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime, result,
                             "vkGetPhysicalDeviceSurfaceFormatsKHR(list)");
    }

// Select a surface format
#ifdef DEBUG
    for (int i = 0; i < formatCount; i++) {
        swapchainCout() << "Supported Format: " << surfaceFormats[i].format
                        << " ColorSpace: " << surfaceFormats[i].colorSpace << std::endl;
    }
#endif
    surfaceFormat_ = chooseSurfaceFormat(surfaceFormats);

    // Find supported present modes
    uint32_t presentModeCount;
    const auto presentCountResult = vkGetPhysicalDeviceSurfacePresentModesKHR(
        physicalDevice_->vkPhysicalDevice(), window_->vkSurface(), &presentModeCount, nullptr);
    if (presentCountResult != VK_SUCCESS ||
        presentModeCount == 0) {
        swapchainCerr() << "failed to get number of supported presentation modes" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime,
                             presentCountResult == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : presentCountResult,
                             "vkGetPhysicalDeviceSurfacePresentModesKHR(count)");
    }

    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    if (const auto result = vkGetPhysicalDeviceSurfacePresentModesKHR(
            physicalDevice_->vkPhysicalDevice(), window_->vkSurface(), &presentModeCount, presentModes.data());
        result != VK_SUCCESS) {
        swapchainCerr() << "failed to get supported presentation modes" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime, result,
                             "vkGetPhysicalDeviceSurfacePresentModesKHR(list)");
    }

    // Choose presentation mode (preferring MAILBOX ~= triple buffering)
    presentMode_ = choosePresentMode(presentModes);

    // Select swap chain size
    int framebufferWidth = 0, framebufferHeight = 0;
    GLFW_GetFramebufferSize(window_->window(), &framebufferWidth, &framebufferHeight);
    extent_ = chooseSwapExtent(surfaceCapabilities, framebufferWidth, framebufferHeight);

    // Determine transformation to use (preferring no transform)
    VkSurfaceTransformFlagBitsKHR surfaceTransform;
    if (surfaceCapabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        surfaceTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    } else {
        surfaceTransform = surfaceCapabilities.currentTransform;
    }

    // Finally, create the swap chain
    VkSwapchainKHR oldSwapchain = swapchain_;

    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = window_->vkSurface();
    createInfo.minImageCount = imageCount_;
    createInfo.imageFormat = surfaceFormat_.format;
    createInfo.imageColorSpace = surfaceFormat_.colorSpace;
    createInfo.imageExtent = extent_;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT; // TODO: cancel VK_IMAGE_USAGE_SAMPLED_BIT
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.queueFamilyIndexCount = 0;
    createInfo.pQueueFamilyIndices = nullptr;
    createInfo.preTransform = surfaceTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode_;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;

    if (const auto result = vkCreateSwapchainKHR(device_->vkDevice(), &createInfo, nullptr, &swapchain_);
        result != VK_SUCCESS) {
        swapchainCerr() << "failed to create swap chain" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime, result, "vkCreateSwapchainKHR");
    } else {
#ifdef DEBUG
        swapchainCout() << "created swap chain" << std::endl;
#endif
    }

    swapchainImages_.clear();
    if (oldSwapchain != VK_NULL_HANDLE) { vkDestroySwapchainKHR(device_->vkDevice(), oldSwapchain, nullptr); }

    // Store the images used by the swap chain
    // Note: these are the images that swap chain image indices refer to
    // Note: actual number of images may differ from requested number, since it's a lower bound
    uint32_t actualImageCount = 0;
    const auto imageCountResult = vkGetSwapchainImagesKHR(device_->vkDevice(), swapchain_, &actualImageCount, nullptr);
    if (imageCountResult != VK_SUCCESS ||
        actualImageCount == 0) {
        swapchainCerr() << "failed to acquire number of swap chain images" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime,
                             imageCountResult == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : imageCountResult,
                             "vkGetSwapchainImagesKHR(count)");
    }
#ifdef DEBUG
    mcvr::log::info("Swapchain") << "actualImageCount: " << actualImageCount << std::endl;
#endif
    imageCount_ = actualImageCount;

    std::vector<VkImage> images(actualImageCount);
    if (const auto result = vkGetSwapchainImagesKHR(device_->vkDevice(), swapchain_, &actualImageCount, images.data());
        result != VK_SUCCESS) {
        swapchainCerr() << "failed to acquire swap chain images" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime, result, "vkGetSwapchainImagesKHR(list)");
    }
    swapchainImages_.clear();
    for (int i = 0; i < actualImageCount; i++) {
        swapchainImages_.push_back(
            SwapchainImage::create(device_, images[i], extent_.width, extent_.height, surfaceFormat_.format));
    }

#ifdef DEBUG
    swapchainCout() << "acquired swap chain images" << std::endl;
#endif
}

vk::Swapchain::~Swapchain() {
    swapchainImages_.clear();
    vkDestroySwapchainKHR(device_->vkDevice(), swapchain_, nullptr);

#ifdef DEBUG
    swapchainCout() << "swapchain deconstructed" << std::endl;
#endif
}

VkSwapchainKHR &vk::Swapchain::vkSwapchain() {
    return swapchain_;
}

VkExtent2D &vk::Swapchain::vkExtent() {
    return extent_;
}

VkExtent2D &vk::Swapchain::vkMaxExtent() {
    return maxExtent_;
}

VkExtent2D &vk::Swapchain::vkMinExtent() {
    return minExtent_;
}

VkSurfaceFormatKHR &vk::Swapchain::vkSurfaceFormat() {
    return surfaceFormat_;
}

std::vector<std::shared_ptr<vk::SwapchainImage>> &vk::Swapchain::swapchainImages() {
    return swapchainImages_;
}

uint32_t vk::Swapchain::imageCount() {
    return imageCount_;
}

bool vk::Swapchain::needsReconstruction() {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_->vkPhysicalDevice(), window_->vkSurface(), &caps) != VK_SUCCESS)
        return true;
    int width = 0, height = 0;
    GLFW_GetFramebufferSize(window_->window(), &width, &height);
    if (width <= 0 || height <= 0) return false;
    auto extent = chooseSwapExtent(caps, width, height);
    if (extent.width != extent_.width || extent.height != extent_.height || imageCount_ < caps.minImageCount ||
        (caps.maxImageCount && imageCount_ > caps.maxImageCount)) return true;
    uint32_t count = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_->vkPhysicalDevice(), window_->vkSurface(), &count, nullptr) != VK_SUCCESS || !count) return true;
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_->vkPhysicalDevice(), window_->vkSurface(), &count, formats.data()) != VK_SUCCESS) return true;
    auto format = chooseSurfaceFormat(formats);
    if (format.format != surfaceFormat_.format || format.colorSpace != surfaceFormat_.colorSpace) return true;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_->vkPhysicalDevice(), window_->vkSurface(), &count, nullptr) != VK_SUCCESS || !count) return true;
    std::vector<VkPresentModeKHR> modes(count);
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_->vkPhysicalDevice(), window_->vkSurface(), &count, modes.data()) != VK_SUCCESS) return true;
    return choosePresentMode(modes) != presentMode_;
}
