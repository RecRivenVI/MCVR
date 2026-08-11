#include "core/render/render_framework.hpp"
#include "core/render/framebuffer_readback_contract.hpp"
#include "core/render/framebuffers.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/renderer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <glm/gtc/packing.hpp>
#include <stdexcept>

namespace {
using mcvr::readback::PixelEncoding;
PixelEncoding colorEncoding(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM: return {1,1,false,false,false};
    case VK_FORMAT_R8G8_UNORM: return {2,1,false,false,false};
    case VK_FORMAT_R8G8B8_UNORM: return {3,1,false,false,false};
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return {4,1,false,false,false};
    case VK_FORMAT_R8G8B8A8_SNORM: return {4,1,false,false,true};
    case VK_FORMAT_R16_SFLOAT: return {1,2,true,true,false};
    case VK_FORMAT_R16G16_SFLOAT: return {2,2,true,true,false};
    case VK_FORMAT_R16G16B16A16_SFLOAT: return {4,2,true,true,false};
    case VK_FORMAT_R32_SFLOAT: return {1,4,true,false,false};
    case VK_FORMAT_R32G32_SFLOAT: return {2,4,true,false,false};
    case VK_FORMAT_R32G32B32A32_SFLOAT: return {4,4,true,false,false};
    default: throw std::invalid_argument("Unsupported color readback format");
    }
}
}

VkResult Framework::readPixels(int x, int y, int width, int height, int format, int type, void *destination) {
    std::lock_guard lock(recreateMtx_);
    if (width < 0 || height < 0 || x < 0 || y < 0 || (width > 0 && height > 0 && !destination))
        throw std::invalid_argument("Invalid framebuffer readback region");
    if (width == 0 || height == 0) return VK_SUCCESS;
    if (type != 0x1401 && type != 0x1406) throw std::invalid_argument("Readback supports unsigned byte or float pixels");
    const bool depth = format == 0x1902, stencil = format == 0x1901;
    int outputChannels;
    switch (format) {
    case 0x1908: outputChannels = 4; break;
    case 0x1907: outputChannels = 3; break;
    case 0x8227: outputChannels = 2; break;
    case 0x1903: case 0x1906: case 0x1902: case 0x1901: outputChannels = 1; break;
    default: throw std::invalid_argument("Unsupported framebuffer pixel component selection");
    }
    auto context = safeAcquireCurrentContext();
    if (!context) return VK_ERROR_INITIALIZATION_FAILED;
    auto ui = pipeline_->acquirePipelineContext(context)->uiModuleContext;
    auto snapshot = ui->framebufferSnapshot(mcvr::framebuffer::READ_FRAMEBUFFER);
    if (snapshot.status != mcvr::framebuffer::FRAMEBUFFER_COMPLETE ||
        static_cast<uint64_t>(x) + width > snapshot.extent.width || static_cast<uint64_t>(y) + height > snapshot.extent.height)
        throw std::invalid_argument("Readback region exceeds the complete framebuffer");
    auto selected = depth || stencil ? snapshot.depthStencil : snapshot.readColor;
    const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (!selected || (selected->aspectMask & aspect) == 0) throw std::runtime_error("Readback attachment is absent");
    auto source = *selected;
    PixelEncoding encoding{};
    if (stencil) encoding = {1,1,false,false,false};
    else if (depth) {
        if (source.format == VK_FORMAT_D32_SFLOAT || source.format == VK_FORMAT_D32_SFLOAT_S8_UINT) encoding = {1,4,true,false,false};
        else if (source.format == VK_FORMAT_D16_UNORM) encoding = {1,2,false,false,false};
        else throw std::invalid_argument("Depth readback requires D16 or D32 storage");
    } else encoding = colorEncoding(source.format);
    auto result = flushForReadback();
    if (result != VK_SUCCESS) return result;
    const size_t sourceStride = static_cast<size_t>(encoding.channels) * encoding.bytes;
    auto buffer = vk::HostVisibleBuffer::create(vma_, device_, static_cast<size_t>(width) * height * sourceStride, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto commands = vk::CommandBuffer::create(device_, vk::CommandPool::create(physicalDevice_, device_));
    const auto initial = source.image->imageLayout();
    const VkImageSubresourceRange range{mcvr::framebuffer::formatAspects(source.format), 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    auto transition = [&](VkImageLayout before, VkImageLayout after) {
        commands->barriersBufferImage({}, {{
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout = before, .newLayout = after,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source.image, .subresourceRange = range,
        }});
    };
    commands->begin();
    transition(initial, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {aspect, source.level, 0, 1};
    copy.imageOffset = {x, static_cast<int>(snapshot.extent.height) - y - height, 0};
    copy.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    vkCmdCopyImageToBuffer(commands->vkCommandBuffer(), source.image->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->vkBuffer(), 1, &copy);
    transition(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, initial);
    commands->end();
    auto fence = vk::Fence::create(device_);
    result = commands->submitMainQueueIndividual(device_, fence);
    if (result != VK_SUCCESS) return recordFailure(result, "vkQueueSubmit(framebuffer pixels)");
    result = vkWaitForFences(device_->vkDevice(), 1, &fence->vkFence(), VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        const auto idle = waitRenderQueueIdle();
        if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST) {
            frameResourceRetainer().retain(commands);
            frameResourceRetainer().retain(buffer);
            frameResourceRetainer().retain(fence);
        }
        return recordFailure(result, "vkWaitForFences(framebuffer pixels)");
    }
    buffer->downloadFromBuffer();
    const auto *pixels = static_cast<const uint8_t *>(buffer->mappedPtr());
    mcvr::readback::convert(pixels, width, height, sourceStride, encoding, format, type, outputChannels, destination);
    return VK_SUCCESS;
}
