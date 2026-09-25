#include "core/diagnostics/frame_profile.hpp"
#include "core/render/textures.hpp"

#include "core/logging.hpp"

#include "core/diagnostics/draw_state_trace.hpp"
#include "core/diagnostics/device_loss_trace.hpp"
#include "core/render/emission.hpp"
#include "core/render/framebuffers.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/upload_retirement.hpp"
#include "core/render/sampler_update.hpp"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <string>

auto texturesCout() {
    return mcvr::log::info("Textures");
}

auto texturesCerr() {
    return mcvr::log::error("Textures");
}

namespace {
void recordTextureState(std::string_view event,
                        uint32_t textureId,
                        uint32_t fallbackId,
                        const std::shared_ptr<vk::DeviceLocalImage> &image,
                        const std::shared_ptr<vk::Sampler> &sampler) noexcept {
    if (!mcvr::diagnostics::drawStateTraceEnabled()) return;
    try {
        // Do not acquire a frame context from a lifecycle observer. That helper may wait for
        // or create a context, changing the timing that this diagnostic is meant to observe.
        const uint32_t frameIndex = UINT32_MAX;
        const bool frameSubmitted = false;
        mcvr::diagnostics::recordTexture(
            event, textureId, fallbackId, frameIndex, frameSubmitted,
            reinterpret_cast<uint64_t>(image.get()), image == nullptr
                ? 0 : mcvr::diagnostics::handleValue(image->vkImage()),
            reinterpret_cast<uint64_t>(sampler.get()), sampler == nullptr
                ? 0 : mcvr::diagnostics::handleValue(sampler->vkSamper()),
            image == nullptr ? 0 : image->width(),
            image == nullptr ? 0 : image->height(),
            image == nullptr ? 0 : image->mipLevels(),
            image == nullptr ? 0 : static_cast<uint32_t>(image->vkFormat()),
            0);
    } catch (...) {
        // Diagnostics must never change the renderer's normal error path.
    }
}
} // namespace

Textures::Textures(std::shared_ptr<Framework> framework) {}

void Textures::reset() {
    textures_.clear();
    samplers.clear();
    attachmentMetadata_.clear();
    frameAliases_.clear();
    releasedTextureFallbacks_.clear();
    if (emission_ != nullptr) { emission_->reset(); }
    resourceReloadActive_ = false;
    resourceReloadRetainedImages_.clear();
    resourceReloadRetainedSamplers_.clear();
    textureNames_.reset();
}

void Textures::resetFrame() {
    auto framework = Renderer::instance().framework();

    collectCompletedUploadsImpl();

    framework->frameResourceRetainer().retain(uploadQueue_);
    uploadQueue_ = std::make_shared<std::map<uint32_t, std::vector<VkBufferImageCopy>>>();
    queuedUploadBytes_ = 0;

    for (auto &entry : caches_) {
        auto &cache = entry.second;
        cache->reset();
    }
}

uint32_t Textures::allocateTexture() {
    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());

    const uint32_t id = textureNames_.allocate();
    releasedTextureFallbacks_.erase(id);
    textures_.emplace(std::make_pair(id, nullptr));
    samplers.emplace(std::make_pair(id, nullptr));
    return id;
}

void Textures::registerFrameAlias(uint32_t id, FrameAliasKind kind) {
    auto framework = Renderer::instance().framework();
    {
        std::scoped_lock lck(mtx_, framework->recreateMtx());
        const auto image = textures_.find(id);
        const auto imageSampler = samplers.find(id);
        if (id == 0 || image == textures_.end() || imageSampler == samplers.end()) {
            throw std::invalid_argument("Frame alias texture name is not allocated");
        }
        if (image->second != nullptr) {
            throw std::logic_error("An initialized texture cannot become a frame alias");
        }
        if (imageSampler->second == nullptr) {
            imageSampler->second = vk::Sampler::create(framework->device(), VK_FILTER_NEAREST,
                                                       VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                                       VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        }
        frameAliases_.insert_or_assign(id, kind);
        attachmentMetadata_.erase(id);
    }
    notifyFrameAlias(id);
}

std::optional<Textures::FrameAliasKind> Textures::frameAliasKind(uint32_t id) const {
    std::lock_guard lock(mtx_);
    const auto alias = frameAliases_.find(id);
    return alias == frameAliases_.end() ? std::nullopt : std::optional{alias->second};
}

void Textures::releaseTexture(uint32_t id, uint32_t fallbackId) {
    auto framework = Renderer::instance().framework();
    bool releasedAlias = false;
    {
        std::scoped_lock lck(mtx_, framework->recreateMtx());
        if (frameAliases_.erase(id) != 0) {
            const auto sampler = samplers.find(id);
            recordTextureState("release.alias", id, fallbackId, nullptr,
                               sampler == samplers.end() ? nullptr : sampler->second);
            if (sampler != samplers.end()) framework->frameResourceRetainer().retain(sampler->second);
            textures_.erase(id);
            samplers.erase(id);
            attachmentMetadata_.erase(id);
            caches_.erase(id);
            if (uploadQueue_ != nullptr) uploadQueue_->erase(id);
            textureNames_.release(id);
            releasedAlias = true;
        }
    }
    if (releasedAlias) {
        if (framework->pipeline() != nullptr && framework->pipeline()->uiModule() != nullptr) {
            framework->pipeline()->uiModule()->unbindTexture(id);
        }
        return;
    }
    std::scoped_lock lck(mtx_, framework->recreateMtx());

    const auto textureIter = textures_.find(id);
    const auto samplerIter = samplers.find(id);
    if (textureIter == textures_.end() || samplerIter == samplers.end()) { return; }

    const auto oldImage = textureIter->second;
    const auto oldSampler = samplerIter->second;
    recordTextureState("release.lookup", id, fallbackId, oldImage, oldSampler);

    const auto fallbackTextureIter = textures_.find(fallbackId);
    const auto fallbackSamplerIter = samplers.find(fallbackId);
    if (id == fallbackId || fallbackTextureIter == textures_.end() || fallbackTextureIter->second == nullptr ||
        fallbackSamplerIter == samplers.end() || fallbackSamplerIter->second == nullptr) {
        return;
    }

    if (resourceReloadActive_) {
        resourceReloadRetainedImages_.push_back(textureIter->second);
        resourceReloadRetainedSamplers_.push_back(samplerIter->second);
    } else {
        // Queue idle covers submitted work only. The current frame's UI command buffers may
        // still be recording against this descriptor generation, so retain both resources
        // until the frame fence retires their slot.
        framework->frameResourceRetainer().retain(textureIter->second);
        framework->frameResourceRetainer().retain(samplerIter->second);
        recordTextureState("release.retained-before-upload", id, fallbackId, oldImage, oldSampler);
        flushQueuedUploadImpl();
        // Submitted uploads retain their actual destination image through their fence. RT
        // and UI draws retain immutable descriptor generations through the frame fence.
        // Changing the CPU binding therefore does not require a queue-idle release barrier.
        framework->pipeline()->bindTexture(fallbackSamplerIter->second, fallbackTextureIter->second, id);
        recordTextureState("release.fallback-bound", id, fallbackId, oldImage, oldSampler);
    }

    releasedTextureFallbacks_[id] = fallbackId;
    textures_.erase(textureIter);
    samplers.erase(samplerIter);
    attachmentMetadata_.erase(id);
    caches_.erase(id);
    if (uploadQueue_ != nullptr) { uploadQueue_->erase(id); }
    if (emission_ != nullptr) { emission_->resetTexture(id); }
    textureNames_.release(id);
    recordTextureState("release.erased", id, fallbackId, oldImage, oldSampler);
}

void Textures::initializeTexture(uint32_t id, uint32_t maxLevel, uint32_t width, uint32_t height, VkFormat format) {
    initializeTextureImpl(id, maxLevel, width, height, format, VK_IMAGE_USAGE_SAMPLED_BIT, 0);
}

void Textures::initializeAttachmentTexture(uint32_t id,
                                           uint32_t maxLevel,
                                           uint32_t width,
                                           uint32_t height,
                                           VkFormat format,
                                           VkImageAspectFlags aspectMask) {
    if (maxLevel == 0 || width == 0 || height == 0) {
        throw std::invalid_argument("Framebuffer attachment texture dimensions and mip count must be non-zero");
    }

    const VkImageAspectFlags formatAspectMask = mcvr::framebuffer::formatAspects(format);
    if (aspectMask == 0 || (aspectMask & formatAspectMask) != aspectMask ||
        (aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0 &&
            (aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
        throw std::invalid_argument("Framebuffer attachment aspect is incompatible with its format");
    }
    // DeviceLocalImage's default view is depth-based for depth/stencil usage. A pure S8 image therefore cannot be
    // represented without extending the common image wrapper.
    if (format == VK_FORMAT_S8_UINT) {
        throw std::invalid_argument("Pure stencil attachment textures are not supported by DeviceLocalImage");
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    usage |= (aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0 ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                                           : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    initializeTextureImpl(id, maxLevel, width, height, format, usage, aspectMask);
}

void Textures::initializeTextureImpl(uint32_t id,
                                     uint32_t maxLevel,
                                     uint32_t width,
                                     uint32_t height,
                                     VkFormat format,
                                     VkImageUsageFlags usage,
                                     VkImageAspectFlags attachmentAspects) {
    auto framework = Renderer::instance().framework();
    auto device = framework->device();
    auto vma = framework->vma();

    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());

    auto textureIter = textures_.find(id);
    if (textureIter == textures_.end()) {
        throw std::runtime_error("The given texture id " + std::to_string(id) + " is not allocated for texture");
    }
    if (frameAliases_.contains(id)) {
        throw std::logic_error("Frame alias storage is owned by the active renderer frame");
    }

    const auto oldImage = textures_[id];
    const auto oldSampler = samplers.contains(id) ? samplers[id] : nullptr;
    if (oldImage != nullptr || oldSampler != nullptr) {
        recordTextureState("initialize.replace-old", id, 0, oldImage, oldSampler);
    }

    if (attachmentAspects != 0) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(framework->physicalDevice()->vkPhysicalDevice(), format, &properties);
        VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        requiredFeatures |= (attachmentAspects & VK_IMAGE_ASPECT_COLOR_BIT) != 0
                                ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
                                : VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ((properties.optimalTilingFeatures & requiredFeatures) != requiredFeatures) {
            throw std::runtime_error("Vulkan format does not support the requested sampled attachment usage");
        }
    }

    if (uploadQueue_ != nullptr && uploadQueue_->contains(id)) { flushQueuedUploadImpl(); }
    if (resourceReloadActive_) {
        if (textures_[id] != nullptr) { resourceReloadRetainedImages_.push_back(textures_[id]); }
    } else {
        framework->frameResourceRetainer().retain(textures_[id]);
    }
#ifdef DEBUG
    if (textures_[id] != nullptr) { mcvr::log::info("Textures") << "Textrue reinitialized: " << id << std::endl; }
#endif
    textures_[id] = vk::DeviceLocalImage::create(device, vma, false, maxLevel, width, height, 1, format, usage, 0,
                                                 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0
#ifdef DEBUG
                                                 ,
                                                 "Texture " + std::to_string(id)
#endif
    );

    attachmentMetadata_.erase(id);
    if (attachmentAspects != 0) {
        AttachmentMetadata metadata{.aspects = attachmentAspects};
        uint32_t viewIndex = 1;
        for (uint32_t level = 0; level < maxLevel; ++level) {
            std::vector<VkImageAspectFlags> views;
            if ((attachmentAspects & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
                views.push_back(VK_IMAGE_ASPECT_COLOR_BIT);
            } else {
                if ((attachmentAspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
                    views.push_back(VK_IMAGE_ASPECT_DEPTH_BIT);
                }
                if ((attachmentAspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
                    views.push_back(VK_IMAGE_ASPECT_STENCIL_BIT);
                }
                if ((attachmentAspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) ==
                    (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
                    views.push_back(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
                }
            }
            for (VkImageAspectFlags viewAspect : views) {
                VkImageViewCreateInfo viewInfo{};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = textures_[id]->vkImage();
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = format;
                viewInfo.subresourceRange = {
                    .aspectMask = viewAspect,
                    .baseMipLevel = level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                };
                textures_[id]->addImageView(viewInfo);
                metadata.viewIndices.emplace(std::make_pair(level, viewAspect), viewIndex++);
            }
        }
        attachmentMetadata_.emplace(id, std::move(metadata));
    }

    auto samplerIter = samplers.find(id);
    if (samplerIter == samplers.end()) {
        throw std::runtime_error("The given texture id " + std::to_string(id) + " is not allocated for sampler");
    }
    if (resourceReloadActive_) {
        if (samplers[id] != nullptr) { resourceReloadRetainedSamplers_.push_back(samplers[id]); }
    } else {
        framework->frameResourceRetainer().retain(samplers[id]);
    }
    samplers[id] =
        vk::Sampler::create(device, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);

    if (!resourceReloadActive_) { bindTextureAndReleasedAliases(id); }
    recordTextureState("initialize.new", id, 0, textures_[id], samplers[id]);
}

void Textures::setSamplingMode(uint32_t id, VkFilter samplingMode, VkSamplerMipmapMode mipmapMode) {
    auto device = Renderer::instance().framework()->device();

    if (frameAliasKind(id).has_value()) {
        {
            std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());
            auto samplerIter = samplers.find(id);
            if (samplerIter == samplers.end() || samplerIter->second == nullptr) {
                throw std::runtime_error("The frame alias id " + std::to_string(id) + " has no sampler");
            }
            if (samplerIter->second->vkSamplingMode() != samplingMode ||
                samplerIter->second->vkMipmapMode() != mipmapMode) {
                auto framework = Renderer::instance().framework();
                recordTextureState("sampler.replace-filter-old", id, 0, nullptr, samplerIter->second);
                framework->frameResourceRetainer().retain(samplerIter->second);
                samplerIter->second = vk::Sampler::create(device, samplingMode, mipmapMode,
                                                          samplerIter->second->vkAddressMode());
                recordTextureState("sampler.replace-filter-new", id, 0, nullptr, samplerIter->second);
            }
        }
        notifyFrameAlias(id);
        return;
    }

    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());

    auto samplerIter = samplers.find(id);
    if (samplerIter == samplers.end() || samplerIter->second == nullptr) {
        throw std::runtime_error("The given texture id " + std::to_string(id) + " has no initialized sampler");
    }
    mcvr::render::updateSampler(samplerIter->second,
        [&](auto &sampler) { return mcvr::render::matchesFilter(sampler, samplingMode, mipmapMode); },
        [&] { return vk::Sampler::create(device, samplingMode, mipmapMode, samplerIter->second->vkAddressMode()); },
        [&](const auto &old) {
            if (resourceReloadActive_) resourceReloadRetainedSamplers_.push_back(old);
            else {
                recordTextureState("sampler.replace-filter-old", id, 0, textures_[id], old);
                Renderer::instance().framework()->frameResourceRetainer().retain(old);
            }
        },
        [&] {
            recordTextureState("sampler.replace-filter-new", id, 0, textures_[id], samplerIter->second);
            if (!resourceReloadActive_) bindTextureAndReleasedAliases(id);
        });
}

void Textures::setAddressMode(uint32_t id, VkSamplerAddressMode addressMode) {
    auto device = Renderer::instance().framework()->device();

    if (frameAliasKind(id).has_value()) {
        {
            std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());
            auto samplerIter = samplers.find(id);
            if (samplerIter == samplers.end() || samplerIter->second == nullptr) {
                throw std::runtime_error("The frame alias id " + std::to_string(id) + " has no sampler");
            }
            if (samplerIter->second->vkAddressMode() != addressMode) {
                auto framework = Renderer::instance().framework();
                recordTextureState("sampler.replace-address-old", id, 0, nullptr, samplerIter->second);
                framework->frameResourceRetainer().retain(samplerIter->second);
                samplerIter->second = vk::Sampler::create(device, samplerIter->second->vkSamplingMode(),
                                                          samplerIter->second->vkMipmapMode(), addressMode);
                recordTextureState("sampler.replace-address-new", id, 0, nullptr, samplerIter->second);
            }
        }
        notifyFrameAlias(id);
        return;
    }

    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());

    auto samplerIter = samplers.find(id);
    if (samplerIter == samplers.end() || samplerIter->second == nullptr) {
        throw std::runtime_error("The given texture id " + std::to_string(id) + " has no initialized sampler");
    }
    mcvr::render::updateSampler(samplerIter->second,
        [&](auto &sampler) { return sampler.vkAddressMode() == addressMode; },
        [&] { return vk::Sampler::create(device, samplerIter->second->vkSamplingMode(),
                                        samplerIter->second->vkMipmapMode(), addressMode); },
        [&](const auto &old) {
            if (resourceReloadActive_) resourceReloadRetainedSamplers_.push_back(old);
            else {
                recordTextureState("sampler.replace-address-old", id, 0, textures_[id], old);
                Renderer::instance().framework()->frameResourceRetainer().retain(old);
            }
        },
        [&] {
            recordTextureState("sampler.replace-address-new", id, 0, textures_[id], samplerIter->second);
            if (!resourceReloadActive_) bindTextureAndReleasedAliases(id);
        });
}

VkResult Textures::beginResourceReload() {
    auto framework = Renderer::instance().framework();
    std::scoped_lock lck(mtx_, framework->recreateMtx());
    if (resourceReloadActive_) { return VK_SUCCESS; }

    flushQueuedUploadImpl();
    const VkResult idleResult = framework->waitRenderQueueIdle();
    if (idleResult != VK_SUCCESS) { return idleResult; }
    collectCompletedUploadsImpl();

    resourceReloadRetainedImages_.clear();
    resourceReloadRetainedSamplers_.clear();
    resourceReloadActive_ = true;
    return VK_SUCCESS;
}

VkResult Textures::endResourceReload() {
    auto framework = Renderer::instance().framework();
    std::scoped_lock lck(mtx_, framework->recreateMtx());
    if (!resourceReloadActive_) { return VK_SUCCESS; }

    flushQueuedUploadImpl();
    const VkResult idleResult = framework->waitRenderQueueIdle();
    if (idleResult != VK_SUCCESS) { return idleResult; }
    collectCompletedUploadsImpl();

    // All descriptor tables are idle now. Publish the complete albedo/PBR generation together,
    // then invalidate temporal consumers before another world frame is recorded.
    bindAllTextures();
    if (framework->pipeline() != nullptr) { framework->pipeline()->onResourceReload(); }

    resourceReloadActive_ = false;
    resourceReloadRetainedImages_.clear();
    resourceReloadRetainedSamplers_.clear();
    return VK_SUCCESS;
}

void Textures::bindTextureAndReleasedAliases(uint32_t id) {
    Renderer::instance().framework()->pipeline()->bindTexture(samplers[id], textures_[id], id);
    for (const auto &[releasedId, fallbackId] : releasedTextureFallbacks_) {
        if (fallbackId == id) {
            Renderer::instance().framework()->pipeline()->bindTexture(samplers[id], textures_[id], releasedId);
        }
    }
}

void Textures::queueUpload(uint8_t *srcPointer,
                           uint32_t srcSizeInBytes,
                           uint32_t srcRowPixels,
                           uint32_t dstId,
                           int srcOffsetX,
                           int srcOffsetY,
                           int dstOffsetX,
                           int dstOffsetY,
                           uint32_t width,
                           uint32_t height,
                           uint32_t level) {
    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());

    auto framework = Renderer::instance().framework();

    auto device = Renderer::instance().framework()->device();
    auto vma = Renderer::instance().framework()->vma();
    auto dstTextureIter = textures_.find(dstId);
    if (dstTextureIter == textures_.end() || dstTextureIter->second == nullptr) {
        throw std::runtime_error("The destination texture id " + std::to_string(dstId) + " is not initialized");
    }
    auto dstTexture = (*dstTextureIter).second;

    if (width == 0 || height == 0) return;
    const uint32_t bytePerPixel = vk::formatToByte(dstTexture->vkFormat());
    const auto packed = mcvr::TextureUploadRegion::checked(srcSizeInBytes, srcRowPixels,
        srcOffsetX, srcOffsetY, width, height, bytePerPixel);
    if (srcPointer == nullptr || level >= dstTexture->mipLevels() || dstOffsetX < 0 || dstOffsetY < 0 ||
        uint64_t(dstOffsetX) + width > std::max(1U, dstTexture->width() >> level) ||
        uint64_t(dstOffsetY) + height > std::max(1U, dstTexture->height() >> level))
        throw std::out_of_range("Texture upload exceeds destination mip");

    auto cacheIter = caches_.find(dstId);
    if (cacheIter == caches_.end()) {
        cacheIter = caches_
                        .emplace(std::make_pair(
                            dstId, ImageBufferCache::create(vma, device, framework->swapchain()->imageCount())))
                        .first;
    }

    auto cache = cacheIter->second;
    size_t offset = cache->appendRegion(srcPointer, packed, bytePerPixel);

    VkBufferImageCopy region = {};
    region.bufferRowLength = 0; // Tightly packed; source skip/pitch has already been applied.
    region.bufferOffset = offset;
    region.imageSubresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1, // avoid VK_REMAINING_ARRAY_LAYERS to get rid of maintaince5
    };
    region.imageSubresource.mipLevel = level;
    region.imageExtent = {width, height, 1};
    region.imageOffset = {dstOffsetX, dstOffsetY, 0};

    auto dstTextureUploadQueueIter = uploadQueue_->find(dstId);
    if (dstTextureUploadQueueIter == uploadQueue_->end()) {
        dstTextureUploadQueueIter = uploadQueue_->emplace(dstId, std::vector<VkBufferImageCopy>{}).first;
    }
    dstTextureUploadQueueIter->second.emplace_back(region);

    queuedUploadBytes_ += packed.packedBytes;
    if (queuedUploadBytes_ >= UPLOAD_FLUSH_THRESHOLD) { flushQueuedUploadImpl(); }
}

void Textures::performQueuedUpload() {
    mcvr::profile::Scope auditProfile("texture-upload-record");
    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());
    collectCompletedUploadsImpl();
    flushQueuedUploadImpl();
}

VkResult Textures::downloadTexture(uint32_t id,
                                   uint32_t level,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t channel,
                                   void *dstPointer) {
    auto framework = Renderer::instance().framework();
    std::scoped_lock lck(mtx_, framework->recreateMtx());

    auto textureIter = textures_.find(id);
    std::shared_ptr<vk::DeviceLocalImage> texture;
    if (frameAliases_.contains(id)) {
        auto attachment = attachmentTexture(id, level, VK_IMAGE_ASPECT_COLOR_BIT);
        if (attachment) texture = attachment->image;
    } else if (textureIter != textures_.end()) texture = textureIter->second;
    if (texture == nullptr || dstPointer == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (mcvr::framebuffer::formatAspects(texture->vkFormat()) != VK_IMAGE_ASPECT_COLOR_BIT)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (level >= texture->mipLevels()) { return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    const uint32_t expectedWidth = std::max(1U, texture->width() >> level);
    const uint32_t expectedHeight = std::max(1U, texture->height() >> level);
    const size_t byteSize = static_cast<size_t>(width) * height * channel;
    if (width != expectedWidth || height != expectedHeight ||
        byteSize != static_cast<size_t>(expectedWidth) * expectedHeight * vk::formatToByte(texture->vkFormat())) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    flushQueuedUploadImpl();
    if (isFramebufferTexture(id)) {
        const auto flush = framework->flushForReadback();
        if (flush != VK_SUCCESS) return flush;
    }
    VkResult result = framework->waitRenderQueueIdle();
    if (result != VK_SUCCESS) { return result; }

    auto device = framework->device();
    auto dstBuffer =
        vk::HostVisibleBuffer::create(framework->vma(), device, byteSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto commandBuffer = vk::CommandBuffer::create(device, vk::CommandPool::create(framework->physicalDevice(), device));
    const VkImageLayout initialLayout = texture->imageLayout();
    const uint32_t mainQueueIndex = framework->physicalDevice()->mainQueueIndex();

    commandBuffer->begin();
    commandBuffer->barriersBufferImage(
        {}, {{
                 .srcStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                 .oldLayout = initialLayout,
                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = texture,
                 .subresourceRange = texture->fullSubresourceRange(),
             }});

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = level;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(commandBuffer->vkCommandBuffer(), texture->vkImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstBuffer->vkBuffer(), 1, &copy);

    commandBuffer
        ->barriersBufferImage(
            {}, {{
                     .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                     .dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                                     VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     .newLayout = initialLayout,
                     .srcQueueFamilyIndex = mainQueueIndex,
                     .dstQueueFamilyIndex = mainQueueIndex,
                     .image = texture,
                     .subresourceRange = texture->fullSubresourceRange(),
                 }})
        ->end();

    auto fence = vk::Fence::create(device);
    result = commandBuffer->submitMainQueueIndividual(device, fence);
    if (result != VK_SUCCESS) { return framework->recordFailure(result, "vkQueueSubmit(texture download)"); }
    result = vkWaitForFences(device->vkDevice(), 1, &fence->vkFence(), true, UINT64_MAX);
    if (result != VK_SUCCESS) {
        const auto idle = framework->waitRenderQueueIdle();
        if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST) {
            framework->frameResourceRetainer().retain(commandBuffer);
            framework->frameResourceRetainer().retain(dstBuffer);
            framework->frameResourceRetainer().retain(fence);
        }
        return framework->recordFailure(result, "vkWaitForFences(texture download)");
    }

    dstBuffer->downloadFromBuffer();
    if (isFramebufferTexture(id)) {
        const size_t rowBytes = static_cast<size_t>(width) * channel;
        for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(static_cast<uint8_t *>(dstPointer) + row * rowBytes,
                static_cast<const uint8_t *>(dstBuffer->mappedPtr()) + (height - 1 - row) * rowBytes, rowBytes);
        }
    } else std::memcpy(dstPointer, dstBuffer->mappedPtr(), byteSize);
    return VK_SUCCESS;
}

std::shared_ptr<vk::HostVisibleBuffer> Textures::acquireUploadStagingBuffer(size_t minSize) {
    auto vma = Renderer::instance().framework()->vma();
    auto device = Renderer::instance().framework()->device();

    for (auto iter = freeUploadStagingBuffers_.begin(); iter != freeUploadStagingBuffers_.end(); ++iter) {
        if ((*iter)->size() >= minSize) {
            auto buffer = *iter;
            freeUploadStagingBuffers_.erase(iter);
            freeUploadStagingBudget_.take(buffer->size());
            return buffer;
        }
    }

    return vk::HostVisibleBuffer::create(vma, device, minSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
}

std::shared_ptr<vk::Fence> Textures::acquireUploadFence() {
    auto device = Renderer::instance().framework()->device();

    if (!freeUploadFences_.empty()) {
        auto fence = freeUploadFences_.back();
        freeUploadFences_.pop_back();
        const VkResult resetResult = vkResetFences(device->vkDevice(), 1, &fence->vkFence());
        if (resetResult != VK_SUCCESS) {
            Renderer::instance().framework()->recordFailure(resetResult, "vkResetFences(texture upload)");
            throw std::runtime_error("Failed to reset texture upload fence: VkResult=" +
                                     std::to_string(resetResult));
        }
        return fence;
    }

    return vk::Fence::create(device);
}

void Textures::collectCompletedUploadsImpl() {
    auto device = Renderer::instance().framework()->device();
    mcvr::render::retireUploads(submittedUploadBatches_,
        [&](const auto &batch) { return vkGetFenceStatus(device->vkDevice(), batch.fence->vkFence()); },
        [&](const auto &batch) {
            freeUploadCommandBuffers_.emplace_back(batch.commandBuffer);
            freeUploadFences_.emplace_back(batch.fence);
            for (auto &stagingBuffer : batch.stagingBuffers) {
                if (freeUploadStagingBudget_.tryRetain(stagingBuffer->size())) {
                    freeUploadStagingBuffers_.emplace_back(stagingBuffer);
                }
            }
        },
        [&](VkResult status) {
            Renderer::instance().framework()->recordFailure(status, "vkGetFenceStatus(texture upload)");
        });
}

void Textures::flushQueuedUploadImpl() {
    mcvr::profile::Scope auditProfile("texture-flush");
    // Poll retirement even after the last upload. Returning first held the final batch forever.
    collectCompletedUploadsImpl();
    if (uploadQueue_ == nullptr || uploadQueue_->empty()) {
        queuedUploadBytes_ = 0;
        return;
    }

    auto framework = Renderer::instance().framework();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();
    std::shared_ptr<vk::CommandBuffer> cmdBuffer;
    if (!freeUploadCommandBuffers_.empty()) {
        cmdBuffer = freeUploadCommandBuffers_.back();
        freeUploadCommandBuffers_.pop_back();
        cmdBuffer->reset();
    } else {
        cmdBuffer = vk::CommandBuffer::create(device, framework->mainCommandPool());
    }
    auto fence = acquireUploadFence();
    cmdBuffer->begin();

    auto mainQueueIndex = physicalDevice->mainQueueIndex();

    std::vector<vk::CommandBuffer::ImageMemoryBarrier> uploadPreImageBarriers, uploadPostImageBarriers;
    for (auto &entry : *uploadQueue_) {
        auto &textureId = entry.first;
        auto textureIter = textures_.find(textureId);
        if (textureIter == textures_.end() || textureIter->second == nullptr) {
            throw std::runtime_error("The queued texture id " + std::to_string(textureId) + " is not initialized");
        }
        auto texture = textureIter->second;
        uploadPreImageBarriers.push_back({
            .srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                            VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                             VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = texture->imageLayout(),
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .image = texture,
            .subresourceRange = vk::wholeColorSubresourceRange,
        });
        texture->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        uploadPostImageBarriers.push_back({
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                            VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .image = texture,
            .subresourceRange = vk::wholeColorSubresourceRange,
        });
        texture->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    cmdBuffer->barriersBufferImage({}, uploadPreImageBarriers);

    for (auto &entry : *uploadQueue_) {
        auto &textureId = entry.first;
        auto &regions = entry.second;

        auto textureIter = textures_.find(textureId);
        if (textureIter == textures_.end() || textureIter->second == nullptr) {
            throw std::runtime_error("The queued texture id " + std::to_string(textureId) + " is not initialized");
        }
        auto texture = textureIter->second;

        auto cacheIter = caches_.find(textureId);
        if (cacheIter == caches_.end()) { continue; }
        auto cache = cacheIter->second;

        vkCmdCopyBufferToImage(cmdBuffer->vkCommandBuffer(), cache->vkBuffer(), texture->vkImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions.size(), regions.data());

        cmdBuffer->barriersBufferImage(
            {}, {{
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = mainQueueIndex,
                    .dstQueueFamilyIndex = mainQueueIndex,
                    .image = texture,
                    .subresourceRange = vk::wholeColorSubresourceRange,
                }});
    }

    cmdBuffer->barriersBufferImage({}, uploadPostImageBarriers);

    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> stagingBuffers;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> destinationImages;
    stagingBuffers.reserve(uploadQueue_->size());
    destinationImages.reserve(uploadQueue_->size());
    for (auto &entry : *uploadQueue_) {
        auto cacheIter = caches_.find(entry.first);
        if (cacheIter == caches_.end()) { continue; }
        auto cache = cacheIter->second;
        cache->flush();
        const size_t replacementCapacity = mcvr::render::replacementUploadCapacity(
            cache->usedSize(), ImageBufferCache::BASE_SIZE);
        auto detachedBuffer = cache->detachCurrentBuffer();
        stagingBuffers.emplace_back(detachedBuffer);
        // A one-time full atlas upload must not make every later animated subregion update
        // allocate another full-atlas staging buffer.
        cache->replaceCurrentBuffer(acquireUploadStagingBuffer(replacementCapacity));
        auto textureIter = textures_.find(entry.first);
        if (textureIter != textures_.end() && textureIter->second != nullptr) {
            destinationImages.emplace_back(textureIter->second);
        }
    }

    cmdBuffer->end();
    const VkResult submitResult = cmdBuffer->submitMainQueueIndividual(device, fence);
    mcvr::diagnostics::device_loss::note("texture-queue-submit", submitResult,
        static_cast<uint32_t>(stagingBuffers.size()));
    if (submitResult != VK_SUCCESS) {
        framework->recordFailure(submitResult, "vkQueueSubmit(texture upload)");
        mcvr::failure::throwIfFatal();
    }

    submittedUploadBatches_.push_back({
        .fence = fence,
        .commandBuffer = cmdBuffer,
        .stagingBuffers = std::move(stagingBuffers),
        .destinationImages = std::move(destinationImages),
    });

    framework->frameResourceRetainer().retain(uploadQueue_);
    uploadQueue_ = std::make_shared<std::map<uint32_t, std::vector<VkBufferImageCopy>>>();
    queuedUploadBytes_ = 0;
}

std::shared_ptr<vk::DeviceLocalImage> Textures::texture(uint32_t id) {
    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());
    auto iter = textures_.find(id);
    return iter != textures_.end() ? iter->second : nullptr;
}

std::shared_ptr<vk::Sampler> Textures::sampler(uint32_t id) {
    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());
    auto iter = samplers.find(id);
    return iter != samplers.end() ? iter->second : nullptr;
}

std::optional<Textures::AttachmentImage>
Textures::attachmentTexture(uint32_t id, uint32_t level, VkImageAspectFlags requiredAspect) {
    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());
    if (auto alias = frameAliases_.find(id); alias != frameAliases_.end()) {
        if (level != 0) return std::nullopt;
        auto framework = Renderer::instance().framework();
        auto frame = framework->safeAcquireCurrentContext();
        if (!frame) return std::nullopt;
        auto ui = framework->pipeline()->acquirePipelineContext(frame)->uiModuleContext;
        const bool depth = alias->second == FrameAliasKind::MainDepth;
        auto allocation = depth ? ui->overlayDrawDepthStencilImage : ui->overlayDrawColorImage;
        if (!allocation || (mcvr::framebuffer::formatAspects(allocation->vkFormat()) & requiredAspect) != requiredAspect)
            return std::nullopt;
        // Aliases sample a per-frame mirror, but an FBO attachment references the
        // actual current-frame image. Resolve on every snapshot, never cache it by ID.
        const uint32_t view = depth && (requiredAspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0
            ? ui->overlayDrawDepthStencilViewIndex : 0;
        return AttachmentImage{.image = allocation, .sampler = samplers.at(id), .viewIndex = view,
            .extent = {allocation->width(), allocation->height()}, .format = allocation->vkFormat(),
            .samples = VK_SAMPLE_COUNT_1_BIT, .aspectMask = requiredAspect};
    }
    const auto image = textures_.find(id);
    const auto imageSampler = samplers.find(id);
    const auto metadata = attachmentMetadata_.find(id);
    if (image == textures_.end() || image->second == nullptr || imageSampler == samplers.end() ||
        imageSampler->second == nullptr || metadata == attachmentMetadata_.end() ||
        (metadata->second.aspects & requiredAspect) != requiredAspect || level >= image->second->mipLevels()) {
        return std::nullopt;
    }
    const auto view = metadata->second.viewIndices.find(std::make_pair(level, requiredAspect));
    if (view == metadata->second.viewIndices.end()) return std::nullopt;

    const uint32_t shift = std::min(level, 31u);
    return AttachmentImage{
        .image = image->second,
        .sampler = imageSampler->second,
        .viewIndex = view->second,
        .extent = {std::max(1u, image->second->width() >> shift),
                   std::max(1u, image->second->height() >> shift)},
        .format = image->second->vkFormat(),
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .aspectMask = requiredAspect,
    };
}

bool Textures::isFramebufferTexture(uint32_t id) const {
    std::lock_guard lock(mtx_);
    return frameAliases_.contains(id) || attachmentMetadata_.contains(id);
}

std::shared_ptr<Emission> Textures::emission() {
    if (!Renderer::options.collectChunkEmission) {
        return nullptr;
    }

    std::scoped_lock lck(mtx_);
    if (emission_ == nullptr) {
        emission_ = Emission::create(std::weak_ptr<Textures>(shared_from_this()));
    }
    return emission_;
}

void Textures::releaseEmission() {
    std::scoped_lock lck(mtx_);
    if (emission_ != nullptr) {
        emission_->reset();
        emission_ = nullptr;
    }
}

void Textures::bindAllTextures() {
    auto device = Renderer::instance().framework()->device();

    std::scoped_lock lck(mtx_);

    for (const auto &[id, texture] : textures_) {
        if (texture == nullptr) {
            continue; // only allocated, but not initialized yet
        }

        Renderer::instance().framework()->pipeline()->bindTexture(samplers[id], texture, id);
    }
    for (const auto &[releasedId, fallbackId] : releasedTextureFallbacks_) {
        const auto textureIter = textures_.find(fallbackId);
        const auto samplerIter = samplers.find(fallbackId);
        if (textureIter != textures_.end() && textureIter->second != nullptr && samplerIter != samplers.end() &&
            samplerIter->second != nullptr) {
            Renderer::instance().framework()->pipeline()->bindTexture(samplerIter->second, textureIter->second,
                                                                      releasedId);
        }
    }
    for (const auto &[id, kind] : frameAliases_) notifyFrameAlias(id);
}

void Textures::bindWorldTextures(std::shared_ptr<WorldPipeline> pipeline) {
    std::scoped_lock lock(mtx_);
    for (const auto &[id, texture] : textures_) {
        const auto sampler = samplers.find(id);
        if (texture && sampler != samplers.end() && sampler->second)
            pipeline->bindTexture(sampler->second, texture, id);
    }
    for (const auto &[releasedId, fallbackId] : releasedTextureFallbacks_) {
        const auto texture = textures_.find(fallbackId);
        const auto sampler = samplers.find(fallbackId);
        if (texture != textures_.end() && texture->second && sampler != samplers.end() && sampler->second)
            pipeline->bindTexture(sampler->second, texture->second, releasedId);
    }
}

void Textures::notifyFrameAlias(uint32_t id) {
    auto framework = Renderer::instance().framework();
    if (framework != nullptr && framework->pipeline() != nullptr && framework->pipeline()->uiModule() != nullptr) {
        std::optional<FrameAliasKind> kind;
        std::shared_ptr<vk::Sampler> aliasSampler;
        {
            std::lock_guard lock(mtx_);
            const auto alias = frameAliases_.find(id);
            const auto imageSampler = samplers.find(id);
            if (alias == frameAliases_.end() || imageSampler == samplers.end() || imageSampler->second == nullptr) {
                return;
            }
            kind = alias->second;
            aliasSampler = imageSampler->second;
        }
        framework->pipeline()->uiModule()->bindFrameAlias(id, *kind == FrameAliasKind::MainDepth, aliasSampler);
    }
}

ImageBufferCache::ImageBufferCache(std::shared_ptr<vk::VMA> vma, std::shared_ptr<vk::Device> device, uint32_t frameNum)
    : vma_(vma), device_(device) {
    capacities_.resize(frameNum);
    bases_.resize(frameNum);
    caches_.resize(frameNum);

    for (int i = 0; i < frameNum; i++) {
        caches_[i] = vk::HostVisibleBuffer::create(vma_, device_, BASE_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        capacities_[i] = BASE_SIZE;
        bases_[i] = 0;
    }
}

ImageBufferCache::~ImageBufferCache() {
#ifdef DEBUG
// mcvr::log::info("Textures") << "ImageBufferCache deconstructed" << std::endl;
#endif
}

size_t ImageBufferCache::appendRegion(const void *src, const mcvr::TextureUploadRegion &region,
                                    size_t texelBytes) {
    const size_t alignment = std::lcm(ALIGNMENT, texelBytes);
    const size_t offset = (bases_[current_] + alignment - 1) / alignment * alignment;
    const size_t size = region.packedBytes;
    if (size > std::numeric_limits<size_t>::max() - offset)
        throw std::overflow_error("Texture staging capacity overflow");
    if (offset + size > capacities_[current_]) {
        size_t newCapacity = std::max(BASE_SIZE, capacities_[current_]);

        while (newCapacity < offset + size) {
            if (newCapacity > std::numeric_limits<size_t>::max() / 2) {
                newCapacity = offset + size;
                break;
            }
            newCapacity *= 2;
        }

        auto newCache = vk::HostVisibleBuffer::create(vma_, device_, newCapacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

        std::memcpy(newCache->mappedPtr(), caches_[current_]->mappedPtr(), bases_[current_]);

        auto framework = Renderer::instance().framework();
        framework->frameResourceRetainer().retain(caches_[current_]);
        caches_[current_] = newCache;
        capacities_[current_] = newCapacity;
    }

    region.copy(static_cast<uint8_t *>(caches_[current_]->mappedPtr()) + offset, src);
    bases_[current_] = offset + size;
    return offset;
}

void ImageBufferCache::flush() {
    caches_[current_]->flush();
}

VkBuffer &ImageBufferCache::vkBuffer() {
    return caches_[current_]->vkBuffer();
}

void ImageBufferCache::reset() {
    current_ = (current_ + 1) % caches_.size();
    bases_[current_] = 0;
}

size_t ImageBufferCache::usedSize() const {
    return bases_[current_];
}

std::shared_ptr<vk::HostVisibleBuffer> ImageBufferCache::detachCurrentBuffer() {
    auto detached = caches_[current_];
    caches_[current_] = nullptr;
    capacities_[current_] = 0;
    bases_[current_] = 0;
    return detached;
}

void ImageBufferCache::replaceCurrentBuffer(std::shared_ptr<vk::HostVisibleBuffer> buffer) {
    caches_[current_] = std::move(buffer);
    capacities_[current_] = caches_[current_]->size();
    bases_[current_] = 0;
}
