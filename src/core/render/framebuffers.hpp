#pragma once

#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/vulkan/image_format.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

class Framework;
class Textures;
namespace vk {
class DeviceLocalImage;
class Sampler;
} // namespace vk

namespace mcvr::framebuffer {
inline constexpr uint32_t NONE = 0;
inline constexpr uint32_t FRAMEBUFFER = 0x8D40;
inline constexpr uint32_t READ_FRAMEBUFFER = 0x8CA8;
inline constexpr uint32_t DRAW_FRAMEBUFFER = 0x8CA9;
inline constexpr uint32_t RENDERBUFFER = 0x8D41;

inline constexpr uint32_t COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr uint32_t DEPTH_ATTACHMENT = 0x8D00;
inline constexpr uint32_t STENCIL_ATTACHMENT = 0x8D20;
inline constexpr uint32_t DEPTH_STENCIL_ATTACHMENT = 0x821A;
inline constexpr uint32_t MAX_COLOR_ATTACHMENTS = 32;

inline constexpr uint32_t FRAMEBUFFER_COMPLETE = 0x8CD5;
inline constexpr uint32_t FRAMEBUFFER_UNDEFINED = 0x8219;
inline constexpr uint32_t FRAMEBUFFER_INCOMPLETE_ATTACHMENT = 0x8CD6;
inline constexpr uint32_t FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT = 0x8CD7;
inline constexpr uint32_t FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER = 0x8CDB;
inline constexpr uint32_t FRAMEBUFFER_INCOMPLETE_READ_BUFFER = 0x8CDC;
inline constexpr uint32_t FRAMEBUFFER_UNSUPPORTED = 0x8CDD;
inline constexpr uint32_t FRAMEBUFFER_INCOMPLETE_MULTISAMPLE = 0x8D56;
inline constexpr uint32_t FRAMEBUFFER_INCOMPLETE_DIMENSIONS = 0x8CD9;

inline bool isColorAttachment(uint32_t attachment) noexcept {
    return attachment >= COLOR_ATTACHMENT0 && attachment < COLOR_ATTACHMENT0 + MAX_COLOR_ATTACHMENTS;
}

inline VkImageAspectFlags attachmentAspect(uint32_t attachment) noexcept {
    if (isColorAttachment(attachment)) return VK_IMAGE_ASPECT_COLOR_BIT;
    switch (attachment) {
    case DEPTH_ATTACHMENT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case STENCIL_ATTACHMENT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case DEPTH_STENCIL_ATTACHMENT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return 0;
    }
}

using vk::formatAspects;

struct AttachmentContract {
    uint32_t attachment = NONE;
    bool allocated = false;
    bool compatible = false;
    uint32_t resourceNamespace = 0;
    uint32_t resourceId = 0;
    uint32_t level = 0;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

inline uint32_t evaluateStatus(std::span<const AttachmentContract> attachments,
                               std::span<const uint32_t> drawBuffers,
                               uint32_t readBuffer) noexcept {
    if (attachments.empty()) return FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;

    const AttachmentContract *first = nullptr;
    const AttachmentContract *depth = nullptr;
    const AttachmentContract *stencil = nullptr;
    for (const auto &attachment : attachments) {
        if (!attachment.allocated || !attachment.compatible || attachment.extent.width == 0 ||
            attachment.extent.height == 0 || attachment.format == VK_FORMAT_UNDEFINED) {
            return FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        }
        if (first == nullptr) {
            first = &attachment;
        } else if (first->extent.width != attachment.extent.width ||
                   first->extent.height != attachment.extent.height) {
            return FRAMEBUFFER_INCOMPLETE_DIMENSIONS;
        } else if (first->samples != attachment.samples) {
            return FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;
        }
        if (attachment.samples != VK_SAMPLE_COUNT_1_BIT) return FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;
        if (attachment.attachment == DEPTH_ATTACHMENT) depth = &attachment;
        if (attachment.attachment == STENCIL_ATTACHMENT) stencil = &attachment;
    }

    if (depth != nullptr && stencil != nullptr &&
        (depth->resourceNamespace != stencil->resourceNamespace || depth->resourceId != stencil->resourceId ||
         depth->level != stencil->level)) {
        return FRAMEBUFFER_UNSUPPORTED;
    }

    auto hasColor = [&](uint32_t attachment) {
        return std::any_of(attachments.begin(), attachments.end(), [&](const auto &candidate) {
            return candidate.attachment == attachment && isColorAttachment(attachment) && candidate.allocated &&
                   candidate.compatible;
        });
    };
    for (uint32_t drawBuffer : drawBuffers) {
        if (drawBuffer != NONE && !hasColor(drawBuffer)) return FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER;
    }
    if (readBuffer != NONE && !hasColor(readBuffer)) return FRAMEBUFFER_INCOMPLETE_READ_BUFFER;
    return FRAMEBUFFER_COMPLETE;
}
} // namespace mcvr::framebuffer

class Framebuffers : public SharedObject<Framebuffers> {
  public:
    enum class StorageResult : uint8_t {
        Success,
        UnknownRenderbuffer,
        InvalidDimensions,
        UnsupportedSamples,
        UnsupportedFormat,
    };

    enum class AttachmentSource : uint8_t { Texture, Renderbuffer };

    struct ResolvedAttachment {
        uint32_t attachment = mcvr::framebuffer::NONE;
        AttachmentSource source = AttachmentSource::Texture;
        uint32_t sourceId = 0;
        uint32_t level = 0;
        std::shared_ptr<vk::DeviceLocalImage> image;
        std::shared_ptr<vk::Sampler> sampler;
        uint32_t viewIndex = 0;
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkImageAspectFlags aspectMask = 0;
    };

    struct Snapshot {
        uint32_t framebufferId = 0;
        uint32_t status = mcvr::framebuffer::FRAMEBUFFER_UNDEFINED;
        VkExtent2D extent{};
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        std::vector<ResolvedAttachment> colors;
        std::optional<ResolvedAttachment> depthStencil;
        std::vector<uint32_t> drawBuffers;
        std::vector<std::optional<ResolvedAttachment>> drawColors;
        uint32_t readBuffer = mcvr::framebuffer::NONE;
        std::optional<ResolvedAttachment> readColor;
    };

    Framebuffers(std::shared_ptr<Framework> framework, std::shared_ptr<Textures> textures);

    uint32_t allocateFramebuffer();
    void deleteFramebuffer(uint32_t id);
    uint32_t allocateRenderbuffer();
    void deleteRenderbuffer(uint32_t id);

    void bindFramebuffer(uint32_t target, uint32_t id);
    void bindRenderbuffer(uint32_t id);
    uint32_t readFramebufferBinding() const;
    uint32_t drawFramebufferBinding() const;
    uint32_t renderbufferBinding() const;

    void framebufferTexture(uint32_t target, uint32_t attachment, uint32_t textureId, uint32_t level);
    void framebufferRenderbuffer(uint32_t target, uint32_t attachment, uint32_t renderbufferId);
    StorageResult renderbufferStorage(uint32_t renderbufferId,
                                      VkFormat format,
                                      uint32_t width,
                                      uint32_t height,
                                      VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void setDrawBuffers(uint32_t target, std::span<const uint32_t> buffers);
    void setReadBuffer(uint32_t target, uint32_t buffer);

    uint32_t checkStatus(uint32_t target) const;
    Snapshot snapshot(uint32_t target) const;

  private:
    struct AttachmentReference {
        AttachmentSource source = AttachmentSource::Texture;
        uint32_t id = 0;
        uint32_t level = 0;

        bool operator==(const AttachmentReference &) const = default;
    };

    struct FramebufferRecord {
        std::map<uint32_t, AttachmentReference> colors;
        std::optional<AttachmentReference> depth;
        std::optional<AttachmentReference> stencil;
        std::vector<uint32_t> drawBuffers{mcvr::framebuffer::COLOR_ATTACHMENT0};
        uint32_t readBuffer = mcvr::framebuffer::COLOR_ATTACHMENT0;
    };

    struct RenderbufferRecord {
        std::shared_ptr<vk::DeviceLocalImage> image;
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkImageAspectFlags aspects = 0;
        std::map<VkImageAspectFlags, uint32_t> viewIndices;
    };

    struct Resolution {
        Snapshot snapshot;
        std::vector<mcvr::framebuffer::AttachmentContract> contracts;
    };

    uint32_t framebufferBindingForTarget(uint32_t target) const;
    FramebufferRecord &boundFramebuffer(uint32_t target);
    const FramebufferRecord &boundFramebuffer(uint32_t target) const;
    static void setAttachment(FramebufferRecord &framebuffer,
                              uint32_t attachment,
                              std::optional<AttachmentReference> reference);
    std::optional<ResolvedAttachment> resolve(const AttachmentReference &reference,
                                              uint32_t attachment) const;
    Resolution resolveFramebuffer(uint32_t target, bool retainResources) const;
    void retain(const std::shared_ptr<vk::DeviceLocalImage> &image) const;
    void retain(const std::shared_ptr<vk::Sampler> &sampler) const;

    std::weak_ptr<Framework> framework_;
    std::weak_ptr<Textures> textures_;
    mutable std::recursive_mutex mtx_;
    std::map<uint32_t, FramebufferRecord> framebuffers_;
    std::map<uint32_t, RenderbufferRecord> renderbuffers_;
    uint32_t nextFramebufferId_ = 1;
    uint32_t nextRenderbufferId_ = 1;
    uint32_t readFramebufferBinding_ = 0;
    uint32_t drawFramebufferBinding_ = 0;
    uint32_t renderbufferBinding_ = 0;
};
