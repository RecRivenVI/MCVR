#include "core/render/modules/ui_module.hpp"

#include "core/render/buffers.hpp"
#include "core/render/modules/world/post_render/post_render_module.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/ui_coverage.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace {
using Attachment = Framebuffers::ResolvedAttachment;

struct MainDepthPushConstant {
    glm::vec4 projection;
    glm::vec2 targetSize;
    glm::vec2 padding;
};
static_assert(sizeof(MainDepthPushConstant) == 32);

void transition(const std::shared_ptr<vk::CommandBuffer> &commands, const Attachment &attachment,
                VkImageLayout layout) {
    // Image currently tracks a single layout; transition the whole allocation consistently.
    const auto aspects = mcvr::framebuffer::formatAspects(attachment.format);
    commands->barriersBufferImage({}, {{
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = attachment.image->imageLayout(),
        .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = attachment.image,
        .subresourceRange = {aspects, 0, VK_REMAINING_MIP_LEVELS, 0, 1},
    }});
    attachment.image->imageLayout() = layout;
}

std::string passKey(const Framebuffers::Snapshot &snapshot) {
    std::ostringstream key;
    for (const auto &color : snapshot.colors) key << color.attachment << ':' << color.format << ';';
    key << "depth=" << (snapshot.depthStencil ? snapshot.depthStencil->format : VK_FORMAT_UNDEFINED);
    key << "/draw=";
    for (auto value : snapshot.drawBuffers) key << value << ',';
    return key.str();
}

// Clip both source and destination at the same interpolation parameter, preserving scaling.
bool clipAxis(int &s0, int &s1, int &d0, int &d1, int sourceSize, int destinationSize) {
    if (s0 == s1 || d0 == d1) return false;
    double low = 0.0, high = 1.0;
    auto clip = [&](int first, int last, int size) {
        double a = (0.0 - first) / (last - first);
        double b = (static_cast<double>(size) - first) / (last - first);
        if (a > b) std::swap(a, b);
        low = std::max(low, a); high = std::min(high, b);
    };
    clip(s0, s1, sourceSize); clip(d0, d1, destinationSize);
    if (low >= high) return false;
    const auto sourceFirst = s0, sourceDelta = s1 - s0, destinationFirst = d0, destinationDelta = d1 - d0;
    s0 = std::clamp(static_cast<int>(std::lround(sourceFirst + sourceDelta * low)), 0, sourceSize);
    s1 = std::clamp(static_cast<int>(std::lround(sourceFirst + sourceDelta * high)), 0, sourceSize);
    d0 = std::clamp(static_cast<int>(std::lround(destinationFirst + destinationDelta * low)), 0, destinationSize);
    d1 = std::clamp(static_cast<int>(std::lround(destinationFirst + destinationDelta * high)), 0, destinationSize);
    return s0 != s1 && d0 != d1;
}
} // namespace

void UIModuleContext::writeMainDepth(std::shared_ptr<WorldPipelineContext> worldContext) {
    if (worldContext == nullptr || overlayDrawDepthStencilImage == nullptr) return;
    std::shared_ptr<PostRenderModuleContext> postRender;
    for (const auto &candidate : worldContext->worldModuleContexts) {
        postRender = std::dynamic_pointer_cast<PostRenderModuleContext>(candidate);
        if (postRender != nullptr) break;
    }
    if (postRender == nullptr || postRender->firstHitDepthImage == nullptr) return;

    auto frame = frameworkContext.lock();
    auto framework = frame->framework.lock();
    auto module = uiModule.lock();
    auto worldBuffer = Renderer::instance().buffers()->worldUniformBuffer();
    if (framework == nullptr || module == nullptr || worldBuffer == nullptr || worldBuffer->mappedPtr() == nullptr) {
        return;
    }
    auto *world = static_cast<vk::Data::WorldUBO *>(worldBuffer->mappedPtr());
    const glm::mat4 &projection = world->cameraProjMat;
    MainDepthPushConstant push{
        .projection = {projection[2][2], projection[3][2], projection[2][3], projection[3][3]},
        .targetSize = {static_cast<float>(overlayDrawDepthStencilImage->width()),
                       static_cast<float>(overlayDrawDepthStencilImage->height())},
    };

    auto descriptor = vk::DescriptorTableBuilder{}
        .beginDescriptorLayoutSet()
        .beginDescriptorLayoutSetBinding()
        .defineDescriptorLayoutSetBinding({
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        })
        .endDescriptorLayoutSetBinding()
        .endDescriptorLayoutSet()
        .definePushConstant({VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MainDepthPushConstant)})
        .build(framework->device());
    descriptor->bindSamplerImage(module->mainDepthSourceSamplers_[frame->frameIndex],
                                 postRender->firstHitDepthImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0, 0);
    framework->frameResourceRetainer().retain(mainDepthDescriptorTable);
    mainDepthDescriptorTable = descriptor;
    module->mainDepthDescriptorTables_[frame->frameIndex] = descriptor;

    Attachment destination{
        .image = overlayDrawDepthStencilImage,
        .viewIndex = overlayDrawDepthStencilViewIndex,
        .extent = {overlayDrawDepthStencilImage->width(), overlayDrawDepthStencilImage->height()},
        .format = overlayDrawDepthStencilImage->vkFormat(),
        .aspectMask = mcvr::framebuffer::formatAspects(overlayDrawDepthStencilImage->vkFormat()),
    };
    // World commands are recorded later but submitted before this overlay buffer.
    // PostRender exports first-hit depth in SHADER_READ_ONLY_OPTIMAL; its current
    // CPU layout belongs to the preceding frame and must not be restored here.
    transition(frame->overlayCommandBuffer, destination, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    frame->overlayCommandBuffer->beginRenderPass({
        .renderPass = module->mainDepthRenderPass_,
        .framebuffer = mainDepthFramebuffer,
        .renderAreaExtent = destination.extent,
    });
    auto commands = frame->overlayCommandBuffer->vkCommandBuffer();
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, module->mainDepthPipeline_->vkPipeline());
    frame->overlayCommandBuffer->bindDescriptorTable(descriptor, VK_PIPELINE_BIND_POINT_GRAPHICS);
    vkCmdPushConstants(commands, descriptor->vkPipelineLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(destination.extent.width),
                        static_cast<float>(destination.extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, destination.extent};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdSetCullMode(commands, VK_CULL_MODE_NONE);
    vkCmdSetPolygonModeEXT(commands, VK_POLYGON_MODE_FILL);
    vkCmdSetDepthBiasEnable(commands, VK_FALSE);
    vkCmdSetDepthTestEnable(commands, VK_TRUE);
    vkCmdSetDepthWriteEnable(commands, VK_TRUE);
    vkCmdSetDepthCompareOp(commands, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(commands, VK_FALSE);
    frame->overlayCommandBuffer->draw(3, 1);
    frame->overlayCommandBuffer->endRenderPass();
    overlayDrawDepthStencilImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    framework->frameResourceRetainer().retain(postRender->firstHitDepthImage);
    framework->frameResourceRetainer().retain(descriptor);
}

void UIModuleContext::captureMainAliases() {
    auto module = uiModule.lock();
    auto frame = frameworkContext.lock();
    auto framework = frame->framework.lock();
    if (module == nullptr || framework == nullptr) return;

    bool captureColor = false;
    bool captureDepth = false;
    {
        std::lock_guard lock(module->overlayDescriptorMutex_);
        for (const auto &[id, binding] : module->overlayTextureBindings_) {
            captureColor |= binding.frameAlias == OverlayTextureBinding::FrameAlias::MainColor;
            captureDepth |= binding.frameAlias == OverlayTextureBinding::FrameAlias::MainDepth;
        }
    }
    if (!captureColor && !captureDepth) return;

    auto commands = frame->overlayCommandBuffer;
    const uint32_t queue = frame->physicalDevice->mainQueueIndex();
    auto copy = [&](const std::shared_ptr<vk::DeviceLocalImage> &source,
                    const std::shared_ptr<vk::DeviceLocalImage> &destination,
                    VkImageAspectFlags aspects, VkImageLayout finalSource, VkImageLayout finalDestination) {
        const VkImageAspectFlags barrierAspects = mcvr::framebuffer::formatAspects(source->vkFormat());
        commands->barriersBufferImage({}, {{
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout = source->imageLayout(),
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = queue,
            .dstQueueFamilyIndex = queue,
            .image = source,
            .subresourceRange = {barrierAspects, 0, 1, 0, 1},
        }, {
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = destination->imageLayout(),
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = queue,
            .dstQueueFamilyIndex = queue,
            .image = destination,
            .subresourceRange = {barrierAspects, 0, 1, 0, 1},
        }});
        VkImageCopy region{};
        region.srcSubresource = {aspects, 0, 0, 1};
        region.dstSubresource = {aspects, 0, 0, 1};
        region.extent = {source->width(), source->height(), 1};
        vkCmdCopyImage(commands->vkCommandBuffer(), source->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       destination->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        source->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        destination->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        Attachment sourceAttachment{.image = source, .format = source->vkFormat(), .aspectMask = aspects};
        Attachment destinationAttachment{.image = destination, .format = destination->vkFormat(),
                                         .aspectMask = aspects};
        transition(commands, sourceAttachment, finalSource);
        transition(commands, destinationAttachment, finalDestination);
        framework->frameResourceRetainer().retain(source);
        framework->frameResourceRetainer().retain(destination);
    };
    if (captureColor) {
        copy(overlayDrawColorImage, mainColorAliasImage, VK_IMAGE_ASPECT_COLOR_BIT,
#ifdef USE_AMD
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
#else
             VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
#endif
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (captureDepth) {
        copy(overlayDrawDepthStencilImage, mainDepthAliasImage, VK_IMAGE_ASPECT_DEPTH_BIT,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
}

Framebuffers::Snapshot UIModuleContext::framebufferSnapshot(uint32_t target) const {
    auto registry = Renderer::instance().framebuffers();
    if (!registry) throw std::runtime_error("Framebuffer registry is not initialized");
    auto snapshot = registry->snapshot(target);
    if (snapshot.framebufferId != 0) return snapshot;
    // Default raster attachment is the compositor's owned UI surface. Resolving the full
    // Minecraft scene for external consumers is separate from this draw attachment.
    if (!overlayDrawColorImage || !overlayDrawDepthStencilImage) return snapshot;
    snapshot.extent = {overlayDrawColorImage->width(), overlayDrawColorImage->height()};
    snapshot.colors.push_back({
        .attachment = mcvr::framebuffer::COLOR_ATTACHMENT0,
        .image = overlayDrawColorImage,
        .extent = snapshot.extent,
        .format = overlayDrawColorImage->vkFormat(),
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    });
    snapshot.depthStencil = Attachment{
        .attachment = mcvr::framebuffer::DEPTH_STENCIL_ATTACHMENT,
        .image = overlayDrawDepthStencilImage,
        .viewIndex = overlayDrawDepthStencilViewIndex,
        .extent = snapshot.extent,
        .format = overlayDrawDepthStencilImage->vkFormat(),
        .aspectMask = mcvr::framebuffer::formatAspects(overlayDrawDepthStencilImage->vkFormat()),
    };
    snapshot.drawBuffers = {mcvr::framebuffer::COLOR_ATTACHMENT0};
    snapshot.drawColors = {snapshot.colors.front()};
    snapshot.readBuffer = mcvr::framebuffer::COLOR_ATTACHMENT0;
    snapshot.readColor = snapshot.colors.front();
    snapshot.status = mcvr::framebuffer::FRAMEBUFFER_COMPLETE;
    return snapshot;
}

VkExtent2D UIModuleContext::drawExtent() const {
    if (overlayMode == FRAMEBUFFER_DRAW) return activeFramebuffer.extent;
    if (overlayMode == DIAGRAM_DRAW && diagramDrawColorImage)
        return {diagramDrawColorImage->width(), diagramDrawColorImage->height()};
    auto registry = Renderer::instance().framebuffers();
    if (registry && registry->drawFramebufferBinding() != 0) {
        auto snapshot = registry->snapshot(mcvr::framebuffer::DRAW_FRAMEBUFFER);
        if (snapshot.status == mcvr::framebuffer::FRAMEBUFFER_COMPLETE) return snapshot.extent;
    }
    return {overlayDrawColorImage->width(), overlayDrawColorImage->height()};
}

uint32_t UIModuleContext::drawAttachmentCount() const {
    return overlayMode == FRAMEBUFFER_DRAW ? static_cast<uint32_t>(activeFramebuffer.drawBuffers.size()) : 1;
}

void UIModuleContext::syncColorAttachments() {
    auto context = frameworkContext.lock();
    uint32_t count = drawAttachmentCount();
    if (count == 0) return;
    std::vector<VkBool32> enabled(count, overlayBlendEnabled);
    auto effectiveEquation = overlayColorBlendEquation;
    // The main UI target reserves alpha for real accumulated GUI coverage used by
    // Streamline DLSS-G recomposition. Preserve Minecraft's RGB blend equation,
    // while accumulating source coverage as A + dstA * (1 - A). Off-screen
    // framebuffers retain their original alpha contract.
    if (overlayMode != FRAMEBUFFER_DRAW && overlayBlendEnabled) {
        mcvr::ui::coverageEquation(effectiveEquation, overlayColorLogicOpEnable);
    }
    std::vector<VkColorBlendEquationEXT> equations(count, effectiveEquation);
    std::vector<VkColorComponentFlags> masks(count, overlayColorWriteMask);
    auto commands = context->overlayCommandBuffer->vkCommandBuffer();
    vkCmdSetColorBlendEnableEXT(commands, 0, count, enabled.data());
    vkCmdSetColorBlendEquationEXT(commands, 0, count, equations.data());
    vkCmdSetColorWriteMaskEXT(commands, 0, count, masks.data());
}

bool UIModuleContext::switchFramebufferDraw() {
    auto registry = Renderer::instance().framebuffers();
    if (!registry || registry->drawFramebufferBinding() == 0) return false;
    if (overlayMode == FRAMEBUFFER_DRAW && activeFramebuffer.framebufferId == registry->drawFramebufferBinding()) return true;
    end();
    activeFramebuffer = registry->snapshot(mcvr::framebuffer::DRAW_FRAMEBUFFER);
    if (activeFramebuffer.status != mcvr::framebuffer::FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Draw to incomplete framebuffer: " + std::to_string(activeFramebuffer.status));
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto module = uiModule.lock();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context->physicalDevice->vkPhysicalDevice(), &properties);
    if (activeFramebuffer.drawBuffers.size() > properties.limits.maxColorAttachments)
        throw std::runtime_error("Framebuffer draw-buffer count exceeds the device limit");
    activeFramebufferPassKey = passKey(activeFramebuffer);
    auto &pass = module->framebufferRenderPasses_[activeFramebufferPassKey];
    if (!pass) {
        vk::RenderPassBuilder builder;
        auto &descriptions = builder.beginAttachmentDescription();
        for (const auto &color : activeFramebuffer.colors) {
            descriptions.defineAttachmentDescription({
                .format = color.format, .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            });
        }
        if (activeFramebuffer.depthStencil) {
            descriptions.defineAttachmentDescription({
                .format = activeFramebuffer.depthStencil->format, .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            });
        }
        descriptions.endAttachmentDescription();
        auto &references = builder.beginAttachmentReference();
        std::vector<uint32_t> colorReferences;
        for (uint32_t slot = 0; slot < activeFramebuffer.drawBuffers.size(); ++slot) {
            uint32_t attachment = VK_ATTACHMENT_UNUSED;
            for (uint32_t i = 0; i < activeFramebuffer.colors.size(); ++i)
                if (activeFramebuffer.colors[i].attachment == activeFramebuffer.drawBuffers[slot]) attachment = i;
            references.defineAttachmentReference({attachment, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
            colorReferences.push_back(slot);
        }
        uint32_t depthReference = static_cast<uint32_t>(-1);
        if (activeFramebuffer.depthStencil) {
            depthReference = colorReferences.size();
            references.defineAttachmentReference({static_cast<uint32_t>(activeFramebuffer.colors.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});
        }
        references.endAttachmentReference();
        builder.beginSubpassDescription().defineSubpassDescription({
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentIndices = colorReferences,
            .depthStencilAttachmentIndex = depthReference,
        }).endSubpassDescription();
        pass = builder.build(framework->device());
    }
    vk::FramebufferBuilder builder;
    auto &attachments = builder.beginAttachment();
    for (const auto &color : activeFramebuffer.colors) {
        transition(context->overlayCommandBuffer, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        attachments.defineAttachment(color.image, color.viewIndex);
        framework->frameResourceRetainer().retain(color.image);
    }
    if (activeFramebuffer.depthStencil) {
        transition(context->overlayCommandBuffer, *activeFramebuffer.depthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        attachments.defineAttachment(activeFramebuffer.depthStencil->image, activeFramebuffer.depthStencil->viewIndex);
        framework->frameResourceRetainer().retain(activeFramebuffer.depthStencil->image);
    }
    attachments.endAttachment();
    activeFramebufferHandle = builder.build(framework->device(), pass, activeFramebuffer.extent.width, activeFramebuffer.extent.height);
    framework->frameResourceRetainer().retain(pass);
    framework->frameResourceRetainer().retain(activeFramebufferHandle);
    context->overlayCommandBuffer->beginRenderPass({
        .renderPass = pass, .framebuffer = activeFramebufferHandle,
        .renderAreaExtent = activeFramebuffer.extent,
    });
    overlayMode = FRAMEBUFFER_DRAW;
    setOverlayScissor(overlayScissorGl[0], overlayScissorGl[1], overlayScissorGl[2], overlayScissorGl[3]);
    syncToCommandBuffer();
    return true;
}

std::shared_ptr<vk::DynamicGraphicsPipeline> UIModuleContext::framebufferPipeline(uint32_t shaderId) {
    auto module = uiModule.lock();
    auto &pipeline = module->framebufferPipelines_[{shaderId, activeFramebufferPassKey}];
    if (!pipeline) {
        const auto &shader = module->overlayDrawShaderInfo(shaderId);
        auto frame = frameworkContext.lock();
        vk::DynamicGraphicsPipelineBuilder builder(drawAttachmentCount());
        auto &stages = builder.defineRenderPass(module->framebufferRenderPasses_.at(activeFramebufferPassKey), 0)
            .beginShaderStage();
        stages.defineShaderStage(shader.shaders.vertexShader, VK_SHADER_STAGE_VERTEX_BIT);
        if (shader.shaders.tessellationControlShader) {
            stages.defineShaderStage(shader.shaders.tessellationControlShader, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
            stages.defineShaderStage(shader.shaders.tessellationEvaluationShader, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
        }
        stages.defineShaderStage(shader.shaders.fragmentShader, VK_SHADER_STAGE_FRAGMENT_BIT).endShaderStage();
        builder.defineVertexInputState(shader.vertexLayout);
        builder.defineInputAssemblyState(shader.topology);
        if (shader.patchControlPoints > 0) builder.definePatchControlPoints(shader.patchControlPoints);
        pipeline = builder.definePipelineLayout(overlayDescriptorTable).build(frame->device);
    }
    return pipeline;
}

void UIModuleContext::endFramebufferDraw() {
    if (overlayMode != FRAMEBUFFER_DRAW) return;
    auto context = frameworkContext.lock();
    context->overlayCommandBuffer->endRenderPass();
    for (const auto &color : activeFramebuffer.colors) transition(context->overlayCommandBuffer, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (activeFramebuffer.depthStencil) transition(context->overlayCommandBuffer, *activeFramebuffer.depthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    bool wroteMain = activeFramebuffer.depthStencil && activeFramebuffer.depthStencil->image == overlayDrawDepthStencilImage;
    for (const auto &color : activeFramebuffer.colors) wroteMain |= color.image == overlayDrawColorImage;
    activeFramebufferHandle.reset();
    activeFramebuffer = {};
    overlayMode = NONE;
    if (wroteMain) captureMainAliases();
}

void UIModuleContext::clearMaskedFramebuffer(bool color, bool depth, bool stencil) {
    switchOverlayDraw();
    const auto snapshot = framebufferSnapshot(mcvr::framebuffer::DRAW_FRAMEBUFFER);
    depth = depth && overlayDepthWriteEnable && snapshot.depthStencil &&
            (snapshot.depthStencil->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT);
    stencil = stencil && (overlayWriteMask[0] & 0xffu) != 0 && snapshot.depthStencil &&
              (snapshot.depthStencil->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT);
    color = color && overlayColorWriteMask != 0 && drawAttachmentCount() != 0;
    if (!color && !depth && !stencil) return;

    auto frame = frameworkContext.lock();
    auto module = uiModule.lock();
    auto commands = frame->overlayCommandBuffer->vkCommandBuffer();
    auto extent = drawExtent();
    VkRect2D scissor = overlayScissorEnabled ? overlayScissor : VkRect2D{{0, 0}, extent};
    if (scissor.extent.width == 0 || scissor.extent.height == 0) return;
    const auto count = drawAttachmentCount();
    const std::string key = overlayMode == FRAMEBUFFER_DRAW ? activeFramebufferPassKey :
                            overlayMode == DIAGRAM_DRAW ? "diagram" : "default";
    auto &pipeline = module->framebufferClearPipelines_[key];
    if (!pipeline) {
        auto renderPass = overlayMode == FRAMEBUFFER_DRAW ? module->framebufferRenderPasses_.at(activeFramebufferPassKey) :
                          overlayMode == DIAGRAM_DRAW ? module->diagramDrawRenderPass_ : module->overlayDrawRenderPass_;
        auto shaderPath = Renderer::folderPath / "shaders/overlay";
        auto vertex = vk::Shader::create(frame->device, (shaderPath / "clear_vert.spv").string());
        auto fragmentFile = count <= 1 ? "clear_frag.spv" : "clear_" + std::to_string(count) + "_frag.spv";
        auto fragment = vk::Shader::create(frame->device, (shaderPath / fragmentFile).string());
        vk::DynamicGraphicsPipelineBuilder builder(count);
        builder.defineRenderPass(renderPass, 0).beginShaderStage()
            .defineShaderStage(vertex, VK_SHADER_STAGE_VERTEX_BIT)
            .defineShaderStage(fragment, VK_SHADER_STAGE_FRAGMENT_BIT).endShaderStage();
        vk::VertexLayoutInfo emptyLayout{};
        builder.defineVertexInputState(emptyLayout);
        pipeline = builder.defineInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .definePipelineLayout(overlayDescriptorTable).build(frame->device);
    }
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->vkPipeline());
    const float clearDepth = std::clamp(overlayClearDepth, 0.0f, 1.0f);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), clearDepth, clearDepth};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdSetCullMode(commands, VK_CULL_MODE_NONE);
    vkCmdSetPolygonModeEXT(commands, VK_POLYGON_MODE_FILL);
    vkCmdSetDepthBiasEnable(commands, VK_FALSE);
    vkCmdSetDepthTestEnable(commands, depth);
    vkCmdSetDepthWriteEnable(commands, depth);
    vkCmdSetDepthCompareOp(commands, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(commands, stencil);
    vkCmdSetStencilOp(commands, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_REPLACE,
                      VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_AND_BACK, overlayClearStencil);
    vkCmdSetStencilCompareMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, ~0u);
    vkCmdSetStencilWriteMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, overlayWriteMask[0]);
    if (frame->device->hasExtendedDynamicState2LogicOp()) vkCmdSetLogicOpEnableEXT(commands, VK_FALSE);
    if (count > 0) {
        const VkColorBlendEquationEXT replaceWithConstant{
            VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
            VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
        std::vector<VkBool32> enabled(count, VK_TRUE);
        std::vector<VkColorBlendEquationEXT> equations(count, replaceWithConstant);
        std::vector<VkColorComponentFlags> masks(count, color ? overlayColorWriteMask : 0);
        std::array<float, 4> values = overlayClearColors;
        for (auto &value : values) value = std::clamp(value, 0.0f, 1.0f);
        vkCmdSetBlendConstants(commands, values.data());
        vkCmdSetColorBlendEnableEXT(commands, 0, count, enabled.data());
        vkCmdSetColorBlendEquationEXT(commands, 0, count, equations.data());
        vkCmdSetColorWriteMaskEXT(commands, 0, count, masks.data());
    }
    frame->overlayCommandBuffer->draw(3, 1);
    syncToCommandBuffer();
}

void UIModuleContext::blitFramebuffer(int sx0, int sy0, int sx1, int sy1,
                                     int dx0, int dy0, int dx1, int dy1, int mask, int filter) {
    constexpr int COLOR = 0x4000, DEPTH = 0x0100, STENCIL = 0x0400;
    if ((mask & ~(COLOR | DEPTH | STENCIL)) != 0 || (filter != 0x2600 && filter != 0x2601))
        throw std::invalid_argument("Invalid framebuffer blit mask or filter");
    if ((mask & (DEPTH | STENCIL)) && filter != 0x2600)
        throw std::invalid_argument("Depth/stencil blits require nearest filtering");
    end();
    auto source = framebufferSnapshot(mcvr::framebuffer::READ_FRAMEBUFFER);
    auto destination = framebufferSnapshot(mcvr::framebuffer::DRAW_FRAMEBUFFER);
    if (source.status != mcvr::framebuffer::FRAMEBUFFER_COMPLETE || destination.status != mcvr::framebuffer::FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Blit requires complete source and destination framebuffers");
    if ((mask & COLOR) && source.readColor) {
        for (const auto &target : destination.drawColors) {
            if (target) blitAttachment(*source.readColor, *target, sx0, sy0, sx1, sy1,
                                       dx0, dy0, dx1, dy1, false, filter);
        }
    }
    if ((mask & DEPTH) && source.depthStencil && destination.depthStencil) {
        blitAttachment(*source.depthStencil, *destination.depthStencil, sx0, sy0, sx1, sy1,
                         dx0, dy0, dx1, dy1, true, filter);
    }
    // Color and depth use the original mapping, without integer endpoint rounding.
    // The remaining transfer path is only for stencil.
    if (!(mask & STENCIL)) return;
    if (!clipAxis(sx0, sx1, dx0, dx1, source.extent.width, destination.extent.width) ||
        !clipAxis(sy0, sy1, dy0, dy1, source.extent.height, destination.extent.height)) return;
    auto frame = frameworkContext.lock();
    auto framework = frame->framework.lock();
    auto copy = [&](const Attachment &src, const Attachment &dst, VkImageAspectFlags aspect) {
        if ((aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && src.format != dst.format)
            throw std::runtime_error("Depth/stencil format conversion requires a shader resolve");
        bool sameImage = src.image->vkImage() == dst.image->vkImage();
        auto sourceLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        auto destinationLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        transition(frame->overlayCommandBuffer, src, sourceLayout);
        transition(frame->overlayCommandBuffer, dst, destinationLayout);
        VkImageBlit region{};
        region.srcSubresource = {aspect, src.level, 0, 1};
        region.dstSubresource = {aspect, dst.level, 0, 1};
        region.srcOffsets[0] = {sx0, static_cast<int>(source.extent.height) - sy0, 0};
        region.srcOffsets[1] = {sx1, static_cast<int>(source.extent.height) - sy1, 1};
        region.dstOffsets[0] = {dx0, static_cast<int>(destination.extent.height) - dy0, 0};
        region.dstOffsets[1] = {dx1, static_cast<int>(destination.extent.height) - dy1, 1};
        vkCmdBlitImage(frame->overlayCommandBuffer->vkCommandBuffer(), src.image->vkImage(), sourceLayout,
                      dst.image->vkImage(), destinationLayout, 1, &region, filter == 0x2601 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        framework->frameResourceRetainer().retain(src.image);
        framework->frameResourceRetainer().retain(dst.image);
        transition(frame->overlayCommandBuffer, src, aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        transition(frame->overlayCommandBuffer, dst, aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    };
    if (source.depthStencil && destination.depthStencil) {
        copy(*source.depthStencil, *destination.depthStencil, VK_IMAGE_ASPECT_STENCIL_BIT);
    }
}
