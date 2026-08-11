#include "core/render/framebuffers.hpp"

#include "core/render/render_framework.hpp"
#include "core/render/textures.hpp"
#include "core/vulkan/image.hpp"
#include "core/vulkan/physical_device.hpp"

#include <stdexcept>
#include <string>
#include <unordered_set>

Framebuffers::Framebuffers(std::shared_ptr<Framework> framework, std::shared_ptr<Textures> textures)
    : framework_(framework), textures_(textures) {}

uint32_t Framebuffers::allocateFramebuffer() {
    std::lock_guard lock(mtx_);
    if (nextFramebufferId_ == 0) throw std::overflow_error("Framebuffer name space exhausted");
    const uint32_t id = nextFramebufferId_++;
    framebuffers_.emplace(id, FramebufferRecord{});
    return id;
}

void Framebuffers::deleteFramebuffer(uint32_t id) {
    if (id == 0) return;
    std::lock_guard lock(mtx_);
    framebuffers_.erase(id);
    if (readFramebufferBinding_ == id) readFramebufferBinding_ = 0;
    if (drawFramebufferBinding_ == id) drawFramebufferBinding_ = 0;
}

uint32_t Framebuffers::allocateRenderbuffer() {
    std::lock_guard lock(mtx_);
    if (nextRenderbufferId_ == 0) throw std::overflow_error("Renderbuffer name space exhausted");
    const uint32_t id = nextRenderbufferId_++;
    renderbuffers_.emplace(id, RenderbufferRecord{});
    return id;
}

void Framebuffers::deleteRenderbuffer(uint32_t id) {
    if (id == 0) return;
    std::lock_guard lock(mtx_);
    const auto existing = renderbuffers_.find(id);
    if (existing != renderbuffers_.end()) {
        retain(existing->second.image);
        renderbuffers_.erase(existing);
    }
    if (renderbufferBinding_ == id) renderbufferBinding_ = 0;
}

void Framebuffers::bindFramebuffer(uint32_t target, uint32_t id) {
    std::lock_guard lock(mtx_);
    if (id != 0 && !framebuffers_.contains(id)) {
        throw std::invalid_argument("Cannot bind unknown framebuffer " + std::to_string(id));
    }
    switch (target) {
    case mcvr::framebuffer::FRAMEBUFFER:
        readFramebufferBinding_ = id;
        drawFramebufferBinding_ = id;
        break;
    case mcvr::framebuffer::READ_FRAMEBUFFER:
        readFramebufferBinding_ = id;
        break;
    case mcvr::framebuffer::DRAW_FRAMEBUFFER:
        drawFramebufferBinding_ = id;
        break;
    default:
        throw std::invalid_argument("Unsupported framebuffer bind target");
    }
}

void Framebuffers::bindRenderbuffer(uint32_t id) {
    std::lock_guard lock(mtx_);
    if (id != 0 && !renderbuffers_.contains(id)) {
        throw std::invalid_argument("Cannot bind unknown renderbuffer " + std::to_string(id));
    }
    renderbufferBinding_ = id;
}

uint32_t Framebuffers::readFramebufferBinding() const {
    std::lock_guard lock(mtx_);
    return readFramebufferBinding_;
}

uint32_t Framebuffers::drawFramebufferBinding() const {
    std::lock_guard lock(mtx_);
    return drawFramebufferBinding_;
}

uint32_t Framebuffers::renderbufferBinding() const {
    std::lock_guard lock(mtx_);
    return renderbufferBinding_;
}

uint32_t Framebuffers::framebufferBindingForTarget(uint32_t target) const {
    switch (target) {
    case mcvr::framebuffer::FRAMEBUFFER:
    case mcvr::framebuffer::DRAW_FRAMEBUFFER:
        return drawFramebufferBinding_;
    case mcvr::framebuffer::READ_FRAMEBUFFER:
        return readFramebufferBinding_;
    default:
        throw std::invalid_argument("Unsupported framebuffer target");
    }
}

Framebuffers::FramebufferRecord &Framebuffers::boundFramebuffer(uint32_t target) {
    const uint32_t id = framebufferBindingForTarget(target);
    if (id == 0) throw std::logic_error("Default framebuffer attachments are supplied by the active render context");
    const auto framebuffer = framebuffers_.find(id);
    if (framebuffer == framebuffers_.end()) throw std::logic_error("Bound framebuffer no longer exists");
    return framebuffer->second;
}

const Framebuffers::FramebufferRecord &Framebuffers::boundFramebuffer(uint32_t target) const {
    const uint32_t id = framebufferBindingForTarget(target);
    if (id == 0) throw std::logic_error("Default framebuffer attachments are supplied by the active render context");
    const auto framebuffer = framebuffers_.find(id);
    if (framebuffer == framebuffers_.end()) throw std::logic_error("Bound framebuffer no longer exists");
    return framebuffer->second;
}

void Framebuffers::setAttachment(FramebufferRecord &framebuffer,
                                 uint32_t attachment,
                                 std::optional<AttachmentReference> reference) {
    if (mcvr::framebuffer::isColorAttachment(attachment)) {
        if (reference.has_value()) {
            framebuffer.colors.insert_or_assign(attachment, *reference);
        } else {
            framebuffer.colors.erase(attachment);
        }
        return;
    }
    switch (attachment) {
    case mcvr::framebuffer::DEPTH_ATTACHMENT:
        framebuffer.depth = reference;
        return;
    case mcvr::framebuffer::STENCIL_ATTACHMENT:
        framebuffer.stencil = reference;
        return;
    case mcvr::framebuffer::DEPTH_STENCIL_ATTACHMENT:
        framebuffer.depth = reference;
        framebuffer.stencil = reference;
        return;
    default:
        throw std::invalid_argument("Unsupported framebuffer attachment point");
    }
}

void Framebuffers::framebufferTexture(uint32_t target,
                                      uint32_t attachment,
                                      uint32_t textureId,
                                      uint32_t level) {
    std::lock_guard lock(mtx_);
    setAttachment(boundFramebuffer(target), attachment,
                  textureId == 0 ? std::nullopt
                                 : std::optional{AttachmentReference{
                                       .source = AttachmentSource::Texture,
                                       .id = textureId,
                                       .level = level,
                                   }});
}

void Framebuffers::framebufferRenderbuffer(uint32_t target,
                                           uint32_t attachment,
                                           uint32_t renderbufferId) {
    std::lock_guard lock(mtx_);
    if (renderbufferId != 0 && !renderbuffers_.contains(renderbufferId)) {
        throw std::invalid_argument("Cannot attach unknown renderbuffer " + std::to_string(renderbufferId));
    }
    setAttachment(boundFramebuffer(target), attachment,
                  renderbufferId == 0 ? std::nullopt
                                      : std::optional{AttachmentReference{
                                            .source = AttachmentSource::Renderbuffer,
                                            .id = renderbufferId,
                                            .level = 0,
                                        }});
}

Framebuffers::StorageResult Framebuffers::renderbufferStorage(uint32_t renderbufferId,
                                                               VkFormat format,
                                                               uint32_t width,
                                                               uint32_t height,
                                                               VkSampleCountFlagBits samples) {
    std::lock_guard lock(mtx_);
    const auto renderbuffer = renderbuffers_.find(renderbufferId);
    if (renderbufferId == 0 || renderbuffer == renderbuffers_.end()) return StorageResult::UnknownRenderbuffer;
    if (width == 0 || height == 0) return StorageResult::InvalidDimensions;
    if (samples != VK_SAMPLE_COUNT_1_BIT) return StorageResult::UnsupportedSamples;

    const VkImageAspectFlags aspects = mcvr::framebuffer::formatAspects(format);
    // DeviceLocalImage creates a depth default view for all depth/stencil usage, so a pure S8 image is rejected.
    if (aspects == 0 || format == VK_FORMAT_S8_UINT) return StorageResult::UnsupportedFormat;

    const auto framework = framework_.lock();
    if (framework == nullptr || framework->physicalDevice() == nullptr || framework->device() == nullptr ||
        framework->vma() == nullptr) {
        throw std::runtime_error("Framebuffer renderbuffer storage requires an active Vulkan framework");
    }
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(framework->physicalDevice()->vkPhysicalDevice(), format, &properties);
    const VkFormatFeatureFlags feature = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | ((aspects & VK_IMAGE_ASPECT_COLOR_BIT) != 0
                                             ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
                                             : VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    if ((properties.optimalTilingFeatures & feature) != feature) return StorageResult::UnsupportedFormat;

    const VkImageUsageFlags usage = (aspects & VK_IMAGE_ASPECT_COLOR_BIT) != 0
                                        ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                        : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    auto image = vk::DeviceLocalImage::create(framework->device(), framework->vma(), false, width, height, 1, format,
                                              usage | VK_IMAGE_USAGE_SAMPLED_BIT
#ifdef DEBUG
                                              ,
                                              "Renderbuffer " + std::to_string(renderbufferId)
#endif
    );

    RenderbufferRecord replacement{
        .image = image,
        .extent = {width, height},
        .format = format,
        .samples = samples,
        .aspects = aspects,
    };
    if ((aspects & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
        replacement.viewIndices.emplace(VK_IMAGE_ASPECT_COLOR_BIT, 0);
    } else {
        if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
            replacement.viewIndices.emplace(VK_IMAGE_ASPECT_DEPTH_BIT, 0);
        }
        auto addView = [&](VkImageAspectFlags aspect) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image->vkImage();
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange = {
                .aspectMask = aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };
            image->addImageView(viewInfo);
            replacement.viewIndices.emplace(aspect, static_cast<uint32_t>(replacement.viewIndices.size()));
        };
        if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) addView(VK_IMAGE_ASPECT_STENCIL_BIT);
        if (aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) addView(aspects);
    }

    retain(renderbuffer->second.image);
    renderbuffer->second = std::move(replacement);
    return StorageResult::Success;
}

void Framebuffers::setDrawBuffers(uint32_t target, std::span<const uint32_t> buffers) {
    if (buffers.size() > mcvr::framebuffer::MAX_COLOR_ATTACHMENTS) {
        throw std::invalid_argument("Too many framebuffer draw buffers");
    }
    for (uint32_t buffer : buffers) {
        if (buffer != mcvr::framebuffer::NONE && !mcvr::framebuffer::isColorAttachment(buffer)) {
            throw std::invalid_argument("Draw buffer must be GL_NONE or a color attachment");
        }
    }
    std::lock_guard lock(mtx_);
    auto &framebuffer = boundFramebuffer(target);
    framebuffer.drawBuffers.assign(buffers.begin(), buffers.end());
}

void Framebuffers::setReadBuffer(uint32_t target, uint32_t buffer) {
    if (buffer != mcvr::framebuffer::NONE && !mcvr::framebuffer::isColorAttachment(buffer)) {
        throw std::invalid_argument("Read buffer must be GL_NONE or a color attachment");
    }
    std::lock_guard lock(mtx_);
    boundFramebuffer(target).readBuffer = buffer;
}

std::optional<Framebuffers::ResolvedAttachment>
Framebuffers::resolve(const AttachmentReference &reference, uint32_t attachment) const {
    const VkImageAspectFlags requiredAspect = mcvr::framebuffer::attachmentAspect(attachment);
    if (requiredAspect == 0 || reference.id == 0) return std::nullopt;

    if (reference.source == AttachmentSource::Texture) {
        const auto textures = textures_.lock();
        if (textures == nullptr) return std::nullopt;
        const auto resolved = textures->attachmentTexture(reference.id, reference.level, requiredAspect);
        if (!resolved.has_value()) return std::nullopt;
        return ResolvedAttachment{
            .attachment = attachment,
            .source = AttachmentSource::Texture,
            .sourceId = reference.id,
            .level = reference.level,
            .image = resolved->image,
            .sampler = resolved->sampler,
            .viewIndex = resolved->viewIndex,
            .extent = resolved->extent,
            .format = resolved->format,
            .samples = resolved->samples,
            .aspectMask = resolved->aspectMask,
        };
    }

    const auto renderbuffer = renderbuffers_.find(reference.id);
    if (renderbuffer == renderbuffers_.end() || renderbuffer->second.image == nullptr ||
        (renderbuffer->second.aspects & requiredAspect) != requiredAspect) {
        return std::nullopt;
    }
    const auto view = renderbuffer->second.viewIndices.find(requiredAspect);
    if (view == renderbuffer->second.viewIndices.end()) return std::nullopt;
    return ResolvedAttachment{
        .attachment = attachment,
        .source = AttachmentSource::Renderbuffer,
        .sourceId = reference.id,
        .level = 0,
        .image = renderbuffer->second.image,
        .sampler = nullptr,
        .viewIndex = view->second,
        .extent = renderbuffer->second.extent,
        .format = renderbuffer->second.format,
        .samples = renderbuffer->second.samples,
        .aspectMask = requiredAspect,
    };
}

Framebuffers::Resolution Framebuffers::resolveFramebuffer(uint32_t target, bool retainResources) const {
    Resolution result;
    result.snapshot.framebufferId = framebufferBindingForTarget(target);
    if (result.snapshot.framebufferId == 0) {
        result.snapshot.status = mcvr::framebuffer::FRAMEBUFFER_UNDEFINED;
        return result;
    }

    const auto &framebuffer = boundFramebuffer(target);
    result.snapshot.drawBuffers = framebuffer.drawBuffers;
    result.snapshot.readBuffer = framebuffer.readBuffer;

    auto addContract = [&](uint32_t attachment, const AttachmentReference &reference,
                           const std::optional<ResolvedAttachment> &resolved) {
        result.contracts.push_back({
            .attachment = attachment,
            .allocated = resolved.has_value(),
            .compatible = resolved.has_value(),
            .resourceNamespace = static_cast<uint32_t>(reference.source) + 1,
            .resourceId = reference.id,
            .level = reference.level,
            .extent = resolved.has_value() ? resolved->extent : VkExtent2D{},
            .format = resolved.has_value() ? resolved->format : VK_FORMAT_UNDEFINED,
            .samples = resolved.has_value() ? resolved->samples : VK_SAMPLE_COUNT_1_BIT,
        });
    };

    for (const auto &[attachment, reference] : framebuffer.colors) {
        const auto resolved = resolve(reference, attachment);
        addContract(attachment, reference, resolved);
        if (resolved.has_value()) result.snapshot.colors.push_back(*resolved);
    }

    std::optional<ResolvedAttachment> depth;
    std::optional<ResolvedAttachment> stencil;
    if (framebuffer.depth.has_value()) {
        depth = resolve(*framebuffer.depth, mcvr::framebuffer::DEPTH_ATTACHMENT);
        addContract(mcvr::framebuffer::DEPTH_ATTACHMENT, *framebuffer.depth, depth);
    }
    if (framebuffer.stencil.has_value()) {
        stencil = resolve(*framebuffer.stencil, mcvr::framebuffer::STENCIL_ATTACHMENT);
        addContract(mcvr::framebuffer::STENCIL_ATTACHMENT, *framebuffer.stencil, stencil);
    }

    result.snapshot.status = mcvr::framebuffer::evaluateStatus(result.contracts, framebuffer.drawBuffers,
                                                                framebuffer.readBuffer);
    if (result.snapshot.status != mcvr::framebuffer::FRAMEBUFFER_COMPLETE) return result;

    if (framebuffer.depth.has_value() && framebuffer.stencil.has_value()) {
        result.snapshot.depthStencil = resolve(*framebuffer.depth, mcvr::framebuffer::DEPTH_STENCIL_ATTACHMENT);
        if (!result.snapshot.depthStencil.has_value()) {
            result.snapshot.status = mcvr::framebuffer::FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
            return result;
        }
    } else if (depth.has_value()) {
        result.snapshot.depthStencil = depth;
    } else if (stencil.has_value()) {
        result.snapshot.depthStencil = stencil;
    }

    if (!result.snapshot.colors.empty()) {
        result.snapshot.extent = result.snapshot.colors.front().extent;
        result.snapshot.samples = result.snapshot.colors.front().samples;
    } else if (result.snapshot.depthStencil.has_value()) {
        result.snapshot.extent = result.snapshot.depthStencil->extent;
        result.snapshot.samples = result.snapshot.depthStencil->samples;
    }

    for (uint32_t drawBuffer : framebuffer.drawBuffers) {
        if (drawBuffer == mcvr::framebuffer::NONE) {
            result.snapshot.drawColors.push_back(std::nullopt);
            continue;
        }
        const auto color = std::find_if(result.snapshot.colors.begin(), result.snapshot.colors.end(),
                                        [&](const auto &candidate) { return candidate.attachment == drawBuffer; });
        result.snapshot.drawColors.push_back(color == result.snapshot.colors.end()
                                                 ? std::nullopt
                                                 : std::optional<ResolvedAttachment>{*color});
    }
    if (framebuffer.readBuffer != mcvr::framebuffer::NONE) {
        const auto color = std::find_if(result.snapshot.colors.begin(), result.snapshot.colors.end(),
                                        [&](const auto &candidate) {
                                            return candidate.attachment == framebuffer.readBuffer;
                                        });
        if (color != result.snapshot.colors.end()) result.snapshot.readColor = *color;
    }

    if (retainResources) {
        std::unordered_set<const void *> retainedImages;
        std::unordered_set<const void *> retainedSamplers;
        auto retainAttachment = [&](const ResolvedAttachment &attachment) {
            if (attachment.image != nullptr && retainedImages.insert(attachment.image.get()).second) {
                retain(attachment.image);
            }
            if (attachment.sampler != nullptr && retainedSamplers.insert(attachment.sampler.get()).second) {
                retain(attachment.sampler);
            }
        };
        for (const auto &color : result.snapshot.colors) retainAttachment(color);
        if (result.snapshot.depthStencil.has_value()) retainAttachment(*result.snapshot.depthStencil);
    }
    return result;
}

uint32_t Framebuffers::checkStatus(uint32_t target) const {
    std::lock_guard lock(mtx_);
    return resolveFramebuffer(target, false).snapshot.status;
}

Framebuffers::Snapshot Framebuffers::snapshot(uint32_t target) const {
    std::lock_guard lock(mtx_);
    return resolveFramebuffer(target, true).snapshot;
}

void Framebuffers::retain(const std::shared_ptr<vk::DeviceLocalImage> &image) const {
    const auto framework = framework_.lock();
    if (framework != nullptr && image != nullptr) framework->frameResourceRetainer().retain(image);
}

void Framebuffers::retain(const std::shared_ptr<vk::Sampler> &sampler) const {
    const auto framework = framework_.lock();
    if (framework != nullptr && sampler != nullptr) framework->frameResourceRetainer().retain(sampler);
}
