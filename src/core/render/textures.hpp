#pragma once

#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/vulkan/all_core_vulkan.hpp"
#include "core/render/texture_name_pool.hpp"
#include "core/render/upload_staging_budget.hpp"
#include "core/render/texture_upload_region.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

class Framework;
class Emission;

class ImageBufferCache;

class Textures : public SharedObject<Textures> {
    friend class Emission;

  public:
    enum class FrameAliasKind : uint8_t { MainColor, MainDepth };

    struct AttachmentImage {
        std::shared_ptr<vk::DeviceLocalImage> image;
        std::shared_ptr<vk::Sampler> sampler;
        uint32_t viewIndex = 0;
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkImageAspectFlags aspectMask = 0;
    };

    struct SubmittedUploadBatch {
        std::shared_ptr<vk::Fence> fence;
        std::shared_ptr<vk::CommandBuffer> commandBuffer;
        std::vector<std::shared_ptr<vk::HostVisibleBuffer>> stagingBuffers;
        std::vector<std::shared_ptr<vk::DeviceLocalImage>> destinationImages;
    };

    Textures(std::shared_ptr<Framework> framework);

    void reset();
    void resetFrame();
    uint32_t allocateTexture();
    void registerFrameAlias(uint32_t id, FrameAliasKind kind);
    std::optional<FrameAliasKind> frameAliasKind(uint32_t id) const;
    bool isFramebufferTexture(uint32_t id) const;
    void releaseTexture(uint32_t id, uint32_t fallbackId);
    void initializeTexture(uint32_t id, uint32_t maxLevel, uint32_t width, uint32_t height, VkFormat format);
    void initializeAttachmentTexture(uint32_t id,
                                     uint32_t maxLevel,
                                     uint32_t width,
                                     uint32_t height,
                                     VkFormat format,
                                     VkImageAspectFlags aspectMask);
    void setSamplingMode(uint32_t id, VkFilter samplingMode, VkSamplerMipmapMode mipmapMode);
    void setAddressMode(uint32_t id, VkSamplerAddressMode addressMode);
    void queueUpload(uint8_t *srcPointer,
                     uint32_t srcSizeInBytes,
                     uint32_t srcRowPixels,
                     uint32_t dstId,
                     int srcOffsetX,
                     int srcOffsetY,
                     int dstOffsetX,
                     int dstOffsetY,
                     uint32_t width,
                     uint32_t height,
                     uint32_t level);
    void performQueuedUpload();
    VkResult beginResourceReload();
    VkResult endResourceReload();
    VkResult downloadTexture(uint32_t id,
                             uint32_t level,
                             uint32_t width,
                             uint32_t height,
                             uint32_t channel,
                             void *dstPointer);
    void bindAllTextures();
    void bindWorldTextures(std::shared_ptr<class WorldPipeline> pipeline);
    std::shared_ptr<vk::DeviceLocalImage> texture(uint32_t id);
    std::shared_ptr<vk::Sampler> sampler(uint32_t id);
    std::optional<AttachmentImage>
    attachmentTexture(uint32_t id, uint32_t level, VkImageAspectFlags requiredAspect);
    std::shared_ptr<Emission> emission();
    void releaseEmission();

    std::map<uint32_t, std::shared_ptr<vk::DeviceLocalImage>> textures_;
    std::map<uint32_t, std::shared_ptr<vk::Sampler>> samplers;
    std::map<uint32_t, uint32_t> releasedTextureFallbacks_;
    std::shared_ptr<Emission> emission_;
    mcvr::TextureNamePool textureNames_{4096};
    mutable std::recursive_mutex mtx_;

    std::map<uint32_t, std::shared_ptr<ImageBufferCache>> caches_;
    std::shared_ptr<std::map<uint32_t, std::vector<VkBufferImageCopy>>> uploadQueue_;
    std::vector<SubmittedUploadBatch> submittedUploadBatches_;
    std::vector<std::shared_ptr<vk::CommandBuffer>> freeUploadCommandBuffers_;
    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> freeUploadStagingBuffers_;
    mcvr::render::UploadStagingBudget freeUploadStagingBudget_{64 * 1024 * 1024};
    std::vector<std::shared_ptr<vk::Fence>> freeUploadFences_;
    size_t queuedUploadBytes_ = 0;
    bool resourceReloadActive_ = false;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> resourceReloadRetainedImages_;
    std::vector<std::shared_ptr<vk::Sampler>> resourceReloadRetainedSamplers_;

  private:
    struct AttachmentMetadata {
        VkImageAspectFlags aspects = 0;
        std::map<std::pair<uint32_t, VkImageAspectFlags>, uint32_t> viewIndices;
    };

    static constexpr size_t UPLOAD_FLUSH_THRESHOLD = 64 * 1024 * 1024;

    void initializeTextureImpl(uint32_t id,
                               uint32_t maxLevel,
                               uint32_t width,
                               uint32_t height,
                               VkFormat format,
                               VkImageUsageFlags usage,
                               VkImageAspectFlags attachmentAspects);
    std::shared_ptr<vk::HostVisibleBuffer> acquireUploadStagingBuffer(size_t minSize);
    std::shared_ptr<vk::Fence> acquireUploadFence();
    void collectCompletedUploadsImpl();
    void flushQueuedUploadImpl();
    void bindTextureAndReleasedAliases(uint32_t id);
    void notifyFrameAlias(uint32_t id);

    std::map<uint32_t, AttachmentMetadata> attachmentMetadata_;
    std::map<uint32_t, FrameAliasKind> frameAliases_;
};

class ImageBufferCache : public SharedObject<ImageBufferCache> {
  public:
    constexpr static size_t BASE_SIZE = 16 * 1024; // 1KB
    constexpr static size_t ALIGNMENT = 4;

    ImageBufferCache(std::shared_ptr<vk::VMA> vma, std::shared_ptr<vk::Device> device, uint32_t frameNum);
    ~ImageBufferCache();

    size_t appendRegion(const void *src, const mcvr::TextureUploadRegion &region, size_t texelBytes);
    void flush();
    void reset();
    size_t usedSize() const;
    std::shared_ptr<vk::HostVisibleBuffer> detachCurrentBuffer();
    void replaceCurrentBuffer(std::shared_ptr<vk::HostVisibleBuffer> buffer);

    VkBuffer &vkBuffer();

  private:
    std::shared_ptr<vk::VMA> vma_;
    std::shared_ptr<vk::Device> device_;

    uint32_t current_ = 0;
    std::vector<size_t> capacities_;
    std::vector<size_t> bases_;
    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> caches_;
};
