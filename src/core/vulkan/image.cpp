#define STB_IMAGE_IMPLEMENTATION

#include "core/logging.hpp"
#include "core/failure_state.hpp"
#include "core/vulkan/image.hpp"
#include "core/vulkan/image_format.hpp"

#include "core/diagnostics/alloc_trace.hpp"
#include "core/vulkan/buffer.hpp"
#include "core/vulkan/command.hpp"
#include "core/vulkan/device.hpp"
#include "core/vulkan/vma.hpp"

#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

auto imageCout() {
    return mcvr::log::info("Image");
}

auto imageCerr() {
    return mcvr::log::error("Image");
}

namespace {
// Diagnostics must never throw, especially from destructors: formats without a known
// byte size simply contribute zero bytes.
size_t tracedImageBytes(uint32_t width, uint32_t height, uint32_t depth, uint32_t layer,
                        VkFormat format) noexcept {
    try {
        return static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(depth) *
               static_cast<size_t>(layer) * vk::formatToByte(format);
    } catch (...) {
        return 0;
    }
}
} // namespace

VkImageAspectFlags vk::DeviceLocalImage::imageAspectMask(VkImageUsageFlags usage) {
    return (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0 ? VK_IMAGE_ASPECT_DEPTH_BIT :
                                                                        VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageSubresourceRange vk::DeviceLocalImage::makeImageSubresourceRange(VkImageAspectFlags aspectMask,
                                                                        uint32_t mipLevels,
                                                                        uint32_t depth,
                                                                        uint32_t layer) {
    return {
        .aspectMask = aspectMask,
        .baseMipLevel = 0,
        .levelCount = mipLevels,
        .baseArrayLayer = 0,
        .layerCount = depth > 1 ? 1u : layer,
    };
}

size_t
vk::DeviceLocalImage::imageByteSize(uint32_t width, uint32_t height, uint32_t depth, uint32_t layer, VkFormat format) {
    return static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(depth) *
           static_cast<size_t>(layer) * vk::formatToByte(format);
}

vk::SwapchainImage::SwapchainImage(
    std::shared_ptr<Device> device, VkImage image, uint32_t width, uint32_t height, VkFormat format)
    : device_(device), width_(width), height_(height), layer_(1), format_(format), image_(image) {
    VkImageViewCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = image_;
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = format_;
    createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.subresourceRange = wholeColorSubresourceRange;

    if (const auto result = vkCreateImageView(device_->vkDevice(), &createInfo, nullptr, &imageViews_[0]);
        result != VK_SUCCESS) {
        imageCerr() << "failed to create image view for image" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime, result, "vkCreateImageView(swapchain)");
    }
}

vk::SwapchainImage::~SwapchainImage() {
    for (const auto imageView : imageViews_) {
        if (imageView != VK_NULL_HANDLE) { vkDestroyImageView(device_->vkDevice(), imageView, nullptr); }
    }
}

uint32_t vk::SwapchainImage::width() {
    return width_;
}

uint32_t vk::SwapchainImage::height() {
    return height_;
}

uint32_t vk::SwapchainImage::depth() {
    return 1;
}

uint32_t vk::SwapchainImage::layer() {
    return 1;
}

VkFormat &vk::SwapchainImage::vkFormat() {
    return format_;
}

VkImage &vk::SwapchainImage::vkImage() {
    return image_;
}

VkImageView &vk::SwapchainImage::vkImageView(int index) {
    return imageViews_[index];
}

VkImageLayout &vk::SwapchainImage::imageLayout() {
    return imageLayout_;
}

size_t vk::formatToByte(VkFormat format) {
    switch (format) {
        // 1 byte
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT: return 1;

        // 2 bytes
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT: return 2;

        // 4 bytes
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SNORM:
        case VK_FORMAT_B8G8R8A8_UINT:
        case VK_FORMAT_B8G8R8A8_SINT:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT: return 4;

        // 8 bytes
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT: return 8;

        // 12 bytes
        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32_SFLOAT: return 12;

        // 16 bytes
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;

        default: {
            throw std::runtime_error("Format not allowed: " + std::to_string(format));
        }
    }
}

vk::DeviceLocalImage::DeviceLocalImage(std::shared_ptr<Device> device,
                                       std::shared_ptr<VMA> vma,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t layer,
                                       VkFormat format,
                                       VkImageUsageFlags usage
#ifdef DEBUG
                                       ,
                                       std::string debugName
#endif
                                       )
    : vk::DeviceLocalImage(device,
                           vma,
                           width,
                           height,
                           1,
                           layer,
                           format,
                           usage
#ifdef DEBUG
                           ,
                           debugName
#endif
      ) {
}

vk::DeviceLocalImage::DeviceLocalImage(std::shared_ptr<Device> device,
                                       std::shared_ptr<VMA> vma,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t depth,
                                       uint32_t layer,
                                       VkFormat format,
                                       VkImageUsageFlags usage
#ifdef DEBUG
                                       ,
                                       std::string debugName
#endif
                                       )
    : vk::DeviceLocalImage(device,
                           vma,
                           true,
                           width,
                           height,
                           depth,
                           layer,
                           format,
                           usage
#ifdef DEBUG
                           ,
                           debugName
#endif
      ) {
}

vk::DeviceLocalImage::DeviceLocalImage(std::shared_ptr<Device> device,
                                       std::shared_ptr<VMA> vma,
                                       bool persistStaging,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t layer,
                                       VkFormat format,
                                       VkImageUsageFlags usage
#ifdef DEBUG
                                       ,
                                       std::string debugName
#endif
                                       )
    : vk::DeviceLocalImage(device,
                           vma,
                           persistStaging,
                           width,
                           height,
                           1,
                           layer,
                           format,
                           usage
#ifdef DEBUG
                           ,
                           debugName
#endif
      ) {
}

vk::DeviceLocalImage::DeviceLocalImage(std::shared_ptr<Device> device,
                                       std::shared_ptr<VMA> vma,
                                       bool persistStaging,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t depth,
                                       uint32_t layer,
                                       VkFormat format,
                                       VkImageUsageFlags usage
#ifdef DEBUG
                                       ,
                                       std::string debugName
#endif
                                       )
    : vk::DeviceLocalImage(device,
                           vma,
                           persistStaging,
                           1,
                           width,
                           height,
                           depth,
                           layer,
                           format,
                           usage,
                           0,
                           VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                           0
#ifdef DEBUG
                           ,
                           debugName
#endif
      ) {
}


vk::DeviceLocalImage::DeviceLocalImage(std::shared_ptr<Device> device,
                                       std::shared_ptr<VMA> vma,
                                       bool persistStaging,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t layer,
                                       VkFormat format,
                                       VkImageUsageFlags usage,
                                       VmaAllocationCreateFlags allocationFlags,
                                       VmaMemoryUsage vmaUsage,
                                       VkImageCreateFlags imageCreateFlags
#ifdef DEBUG
                                       ,
                                       std::string debugName
#endif
                                       )
    : DeviceLocalImage(device,
                       vma,
                       persistStaging,
                       1,
                       width,
                       height,
                       layer,
                       format,
                       usage,
                       allocationFlags,
                       vmaUsage,
                       imageCreateFlags
#ifdef DEBUG
                       ,
                       debugName
#endif
      ) {
}

vk::DeviceLocalImage::DeviceLocalImage(std::shared_ptr<Device> device,
                                       std::shared_ptr<VMA> vma,
                                       bool persistStaging,
                                       uint32_t mipLevels,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t layer,
                                       VkFormat format,
                                       VkImageUsageFlags usage,
                                       VmaAllocationCreateFlags allocationFlags,
                                       VmaMemoryUsage vmaUsage,
                                       VkImageCreateFlags imageCreateFlags
#ifdef DEBUG
                                       ,
                                       std::string debugName
#endif
                                       )
    : DeviceLocalImage(device,
                       vma,
                       persistStaging,
                       mipLevels,
                       width,
                       height,
                       1,
                       layer,
                       format,
                       usage,
                       allocationFlags,
                       vmaUsage,
                       imageCreateFlags
#ifdef DEBUG
                       ,
                       debugName
#endif
      ) {
}

vk::DeviceLocalImage::DeviceLocalImage(std::shared_ptr<Device> device,
                                       std::shared_ptr<VMA> vma,
                                       bool persistStaging,
                                       uint32_t mipLevels,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t depth,
                                       uint32_t layer,
                                       VkFormat format,
                                       VkImageUsageFlags usage,
                                       VmaAllocationCreateFlags allocationFlags,
                                       VmaMemoryUsage vmaUsage,
                                       VkImageCreateFlags imageCreateFlags
#ifdef DEBUG
                                       ,
                                       std::string debugName
#endif
                                       )
    : device_(device),
      vma_(vma),
      mipLevels_(mipLevels),
      width_(width),
      height_(height),
      depth_(depth),
      layer_(layer),
      format_(format),
      persistStaging_(persistStaging),
      usage_(usage),
      allocationFlags_(allocationFlags),
      vmaUsage_(vmaUsage)
#ifdef DEBUG
      ,
      debugName(debugName)
#endif
    {
    if (depth_ == 0) {
        imageCerr() << "image depth must be at least 1" << std::endl;
        mcvr::failure::invariant("DeviceLocalImage::DeviceLocalImage", "image depth must be at least one");
    }
    if (depth_ > 1 && layer_ != 1) {
        imageCerr() << "3d images do not support array layers in DeviceLocalImage" << std::endl;
        mcvr::failure::invariant("DeviceLocalImage::DeviceLocalImage", "3D image has array layers");
    }
    if (depth_ > 1 && (imageCreateFlags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0) {
        imageCerr() << "3d images cannot be cube compatible" << std::endl;
        mcvr::failure::invariant("DeviceLocalImage::DeviceLocalImage", "3D image is cube compatible");
    }

#ifdef DEBUG
    imageCout() << "Creating image with width: " << width << " height: " << height << " depth: " << depth
                << " layer: " << layer
                << " channel: " << vk::formatToByte(format) << " mip level: " << mipLevels
                << " staging: " << (persistStaging ? "enabled" : "disabled") << std::endl;
#endif

    if (persistStaging_) {
        // staging buffer
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = imageByteSize(width_, height_, depth_, layer_, format_);
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        if (const auto result = vmaCreateBuffer(vma_->allocator(), &bufferInfo, &allocationInfo, &stagingBuffer_,
                &stagingAllocation_, &stagingAllocationInfo_); result != VK_SUCCESS) {
            imageCerr() << "failed to create staging buffer" << std::endl;
            mcvr::failure::raise(mcvr::failure::Kind::runtime, result, "vmaCreateBuffer(image staging)");
        }
        mappedPtr_ = stagingAllocationInfo_.pMappedData;
    }

    // image
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = imageCreateFlags;
    imageInfo.imageType = depth_ > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.format = format_;
    imageInfo.extent = {width_, height_, depth_};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = layer_;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | usage_;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.flags = allocationFlags_;
    allocationInfo.usage = vmaUsage;
    if (const auto result = vmaCreateImage(vma_->allocator(), &imageInfo, &allocationInfo, &image_, &allocation_,
            &allocationInfo_); result != VK_SUCCESS) {
        imageCerr() << "failed to create image" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime, result, "vmaCreateImage");
    }
    allocTraceTag_ = mcvr::diag::recordAllocCreate("image", tracedImageBytes(width_, height_, depth_, layer_, format_));

    VkImageViewCreateInfo createInfo = {};
    auto makeDefaultImageViewType = [](uint32_t depth, uint32_t layer) {
        if (depth > 1) { return VK_IMAGE_VIEW_TYPE_3D; }
        return layer == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    };
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = image_;
    createInfo.viewType = makeDefaultImageViewType(depth_, layer_);
    createInfo.format = format_;
    createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.subresourceRange = makeImageSubresourceRange(imageAspectMask(usage_), mipLevels_, depth_, layer_);

    if (const auto result = vkCreateImageView(device_->vkDevice(), &createInfo, nullptr, &imageViews_[0]);
        result != VK_SUCCESS) {
        imageCerr() << "failed to create image view for image" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime, result, "vkCreateImageView");
    }

#ifdef DEBUG
    imageCout() << "device local image (" << debugName << ") init" << std::endl;
#endif
}

vk::DeviceLocalImage::~DeviceLocalImage() {
    mcvr::diag::recordAllocDestroy("image", tracedImageBytes(width_, height_, depth_, layer_, format_), allocTraceTag_);
    for (const auto imageView : imageViews_) {
        if (imageView != VK_NULL_HANDLE) { vkDestroyImageView(device_->vkDevice(), imageView, nullptr); }
    }
    if (stagingBuffer_ != VK_NULL_HANDLE || stagingAllocation_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(vma_->allocator(), stagingBuffer_, stagingAllocation_);
    }
    if (image_ != VK_NULL_HANDLE || allocation_ != VK_NULL_HANDLE) {
        vmaDestroyImage(vma_->allocator(), image_, allocation_);
    }

#ifdef DEBUG
    imageCout() << "device local image (" << debugName << ") deconstructed" << std::endl;
#endif
}

void vk::DeviceLocalImage::uploadToStagingBuffer(void *src) {
    if (!persistStaging_) {
        if (stagingBuffer_ != VK_NULL_HANDLE || stagingAllocation_ != VK_NULL_HANDLE || mappedPtr_ != nullptr) {
            imageCerr() << "if not persist staging, the staging buffer should not exist!" << std::endl;
            mcvr::failure::invariant("DeviceLocalImage::uploadToStagingBuffer",
                                     "non-persistent staging state is already allocated");
        }

        // staging buffer
        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = imageByteSize(width_, height_, depth_, layer_, format_);
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        if (const auto result = vmaCreateBuffer(vma_->allocator(), &bufferInfo, &allocationInfo, &stagingBuffer_,
                &stagingAllocation_, &stagingAllocationInfo_); result != VK_SUCCESS) {
            imageCerr() << "failed to create staging buffer" << std::endl;
            mcvr::failure::raise(mcvr::failure::Kind::runtime, result, "vmaCreateBuffer(image upload staging)");
        }
        mappedPtr_ = stagingAllocationInfo_.pMappedData;
    }

    size_t size = imageByteSize(width_, height_, depth_, layer_, format_);
#ifdef DEBUG
    imageCout() << "Flushed " << size << " bytes into staging buffer" << std::endl;
#endif
    std::memcpy(mappedPtr_, src, size);
    vmaFlushAllocation(vma_->allocator(), stagingAllocation_, 0, size);

    if (!persistStaging_) {
        vmaDestroyBuffer(vma_->allocator(), stagingBuffer_, stagingAllocation_);
        stagingBuffer_ = VK_NULL_HANDLE;
        stagingAllocation_ = VK_NULL_HANDLE;
        mappedPtr_ = nullptr;
    }
}

void vk::DeviceLocalImage::uploadToImage(VkCommandBuffer cmdBuffer) {
    VkBufferImageCopy region = {};
    region.imageSubresource = {usage_ == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT ?
                                   static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) :
                                   static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
                               0, 0, depth_ > 1 ? 1u : layer_};
    region.imageExtent = {width_, height_, depth_};
    vkCmdCopyBufferToImage(cmdBuffer, stagingBuffer_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void vk::DeviceLocalImage::uploadToImage(std::shared_ptr<CommandBuffer> cmdBuffer) {
    uploadToImage(cmdBuffer->vkCommandBuffer());
}

void vk::DeviceLocalImage::uploadToImage(VkCommandBuffer cmdBuffer,
                                         std::shared_ptr<Buffer> buffer,
                                         std::vector<VkBufferImageCopy> &regions) {
    vkCmdCopyBufferToImage(cmdBuffer, buffer->vkBuffer(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions.size(),
                           regions.data());
}

void vk::DeviceLocalImage::uploadToImage(std::shared_ptr<CommandBuffer> cmdBuffer,
                                         std::shared_ptr<Buffer> buffer,
                                         std::vector<VkBufferImageCopy> &regions) {
    uploadToImage(cmdBuffer->vkCommandBuffer(), buffer, regions);
}

uint32_t vk::DeviceLocalImage::width() {
    return width_;
}

uint32_t vk::DeviceLocalImage::height() {
    return height_;
}

uint32_t vk::DeviceLocalImage::depth() {
    return depth_;
}

uint32_t vk::DeviceLocalImage::layer() {
    return layer_;
}

uint32_t vk::DeviceLocalImage::mipLevels() const {
    return mipLevels_;
}

VkFormat &vk::DeviceLocalImage::vkFormat() {
    return format_;
}

VkBuffer &vk::DeviceLocalImage::vkStagingBuffer() {
    return stagingBuffer_;
}

VkImage &vk::DeviceLocalImage::vkImage() {
    return image_;
}

VkImageView &vk::DeviceLocalImage::vkImageView(int index) {
    return imageViews_[index];
}

VkImageLayout &vk::DeviceLocalImage::imageLayout() {
    return imageLayout_;
}

void *vk::DeviceLocalImage::mappedPtr() {
    return mappedPtr_;
}

VkImageSubresourceRange vk::DeviceLocalImage::fullSubresourceRange() const {
    return formatSubresourceRange(format_, mipLevels_, depth_, layer_);
}

void vk::DeviceLocalImage::addImageView(VkImageViewCreateInfo info) {
    VkImageView vkImageView{};
    if (const auto result = vkCreateImageView(device_->vkDevice(), &info, nullptr, &vkImageView);
        result != VK_SUCCESS) {
        imageCerr() << "failed to create image view for image" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::runtime, result, "vkCreateImageView(additional)");
    }
    imageViews_.push_back(vkImageView);
}

vk::Sampler::Sampler(std::shared_ptr<Device> device)
    : Sampler(device, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT) {}

vk::Sampler::Sampler(std::shared_ptr<Device> device,
                     VkFilter samplingMode,
                     VkSamplerMipmapMode mipmapMode,
                     VkSamplerAddressMode addressMode)
    : device_(device), samplingMode_(samplingMode), mipmapMode_(mipmapMode), addressMode_(addressMode) {
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = samplingMode;                       // 放大时的过滤方式
    samplerInfo.minFilter = samplingMode;                       // 缩小时的过滤方式
    samplerInfo.addressModeU = addressMode;                     // U方向寻址
    samplerInfo.addressModeV = addressMode;                     // V方向寻址
    samplerInfo.addressModeW = addressMode;                     // W方向寻址
    samplerInfo.anisotropyEnable = VK_FALSE;                    // 先不启用各向异性过滤
    samplerInfo.maxAnisotropy = 16.0f;                          // 最大各向异性采样数
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK; // 边界色
    samplerInfo.unnormalizedCoordinates = VK_FALSE;             // 使用标准化坐标 [0,1]
    samplerInfo.compareEnable = VK_FALSE;                       // 禁用深度比较
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = mipmapMode;    // mipmap插值
    samplerInfo.mipLodBias = 0.0f;          // mipmap偏移
    samplerInfo.minLod = 0.0f;              // 最小mip层级
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE; // 最大mip层级（无限制）

    if (vkCreateSampler(device->vkDevice(), &samplerInfo, nullptr, &samper_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sampler!");
    }
}

vk::Sampler::~Sampler() {
    vkDestroySampler(device_->vkDevice(), samper_, nullptr);
}

VkSampler vk::Sampler::vkSamper() {
    return samper_;
}

VkFilter vk::Sampler::vkSamplingMode() {
    return samplingMode_;
}

VkSamplerMipmapMode vk::Sampler::vkMipmapMode() {
    return mipmapMode_;
}

VkSamplerAddressMode vk::Sampler::vkAddressMode() {
    return addressMode_;
}

auto imageLoaderCout() {
    return mcvr::log::info("ImageLoader");
}

auto imageLoaderCerr() {
    return mcvr::log::error("ImageLoader");
}

// 0~1 float
float linearToSrgb(float linear) {
    if (linear <= 0.0031308f) {
        return 12.92f * linear;
    } else {
        return 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
    }
}

// 0~255 uint8_t
uint8_t linearToSrgb(uint8_t linear) {
    if (linear / 255.0f <= 0.0031308f) {
        return 12.92f * linear;
    } else {
        return (1.055f * powf(linear / 255.0f, 1.0f / 2.4f) - 0.055f) * 255.0f;
    }
}

vk::ImageLoader::ImageLoader(std::vector<std::string> imagePaths, uint32_t forceChannel, bool convertLinearToSrgb)
    : imagePaths_(imagePaths), channel_(forceChannel), layer_(imagePaths.size()), data_() {
    if (imagePaths.size() == 0) { imageLoaderCerr() << "Cannot load 0 image" << std::endl; }

    for (int i = 0; i < imagePaths_.size(); i++) {
        stbi_uc *imageData;
        int channel;
        if (i == 0) {
            imageData = stbi_load(imagePaths_[i].c_str(), &width_, &height_, &channel, 0);
#ifdef DEBUG
            imageLoaderCout() << "Loaded image from " << imagePaths_[i] << " with width: " << width_
                              << " height: " << height_ << " channel: " << channel << std::endl;
#endif
        } else {
            int currentWidth, currentHeight;
            imageData = stbi_load(imagePaths_[i].c_str(), &currentWidth, &currentHeight, &channel, 0);
            if (currentWidth != width_ || currentHeight != height_) {
                imageLoaderCerr() << "images are not with the same shape" << std::endl;
                imageLoaderCerr() << "current: [width=" << currentWidth << ", height=" << currentHeight << "]"
                                  << std::endl;
                imageLoaderCerr() << "existing: [width=" << width_ << ", height=" << height_ << "]" << std::endl;
                mcvr::failure::invariant("ImageLoader::load", "images have different dimensions");
            }
#ifdef DEBUG
            imageLoaderCout() << "Loaded image from " << imagePaths_[i] << " with width: " << currentWidth
                              << " height: " << currentHeight << " channel: " << channel << std::endl;
#endif
        }

        if (forceChannel < channel) {
            imageLoaderCerr() << "Cannot compress image" << std::endl;
            mcvr::failure::invariant("ImageLoader::load", "requested channel compression is unsupported");
        }

        for (int h = 0; h < height_; h++) {
            for (int w = 0; w < width_; w++) {
                const int srcIndex = (h * width_ + w) * channel;
                for (int c = 0; c < std::min(channel, 3);
                     c++) { // only do linear to srgb transform for color, not alpha
                    uint8_t value = imageData[srcIndex + c];
                    data_.push_back(convertLinearToSrgb ? linearToSrgb(value) : value);
                }

                if (channel == 3 && forceChannel == 4) {
                    data_.push_back(255);
                } else if (channel == 1 && forceChannel == 4) {
                    uint8_t value = imageData[srcIndex];
                    value = convertLinearToSrgb ? linearToSrgb(value) : value;
                    data_.push_back(value);
                    data_.push_back(value);
                    data_.push_back(255);
                } else if (channel == 4 && forceChannel == 4) {
                    data_.push_back(imageData[srcIndex + 3]);
                } else {
                    imageLoaderCerr() << "Force channel of " << forceChannel << " is not support for channel "
                                      << channel << std::endl;
                    mcvr::failure::invariant("ImageLoader::load", "requested forced channel count is unsupported");
                }
            }
        }

        stbi_image_free(imageData);
    }
}

vk::ImageLoader::~ImageLoader() {}

uint32_t vk::ImageLoader::width() {
    return width_;
}

uint32_t vk::ImageLoader::height() {
    return height_;
}

uint32_t vk::ImageLoader::channel() {
    return channel_;
}

uint32_t vk::ImageLoader::layer() {
    return layer_;
}

void *vk::ImageLoader::data() {
    return data_.data();
}
