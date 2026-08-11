#include "core/render/modules/ui_module.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"

#include <algorithm>
#include <stdexcept>

namespace {
struct BlitPush {
    glm::vec4 sourceRect;
    glm::vec4 destinationRect;
    glm::vec2 sourceSize;
    glm::vec2 destinationSize;
    int sourceLevel;
};
static_assert(offsetof(BlitPush, sourceLevel) == 48);

void blitTransition(const std::shared_ptr<vk::CommandBuffer> &commands,
                    const Framebuffers::ResolvedAttachment &attachment, VkImageLayout layout) {
    commands->barriersBufferImage({}, {{
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = attachment.image->imageLayout(), .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = attachment.image,
        .subresourceRange = {mcvr::framebuffer::formatAspects(attachment.format), 0, VK_REMAINING_MIP_LEVELS, 0, 1},
    }});
    attachment.image->imageLayout() = layout;
}
}

void UIModuleContext::blitAttachment(const Framebuffers::ResolvedAttachment &source,
                                     const Framebuffers::ResolvedAttachment &destination,
                                     int sx0, int sy0, int sx1, int sy1,
                                     int dx0, int dy0, int dx1, int dy1, bool depth, int filter) {
    if (sx0 == sx1 || sy0 == sy1 || dx0 == dx1 || dy0 == dy1) return;
    auto frame = frameworkContext.lock();
    auto framework = frame->framework.lock();
    auto module = uiModule.lock();
    const auto extent = destination.extent;
    int left = std::max(0, std::min(dx0, dx1));
    int right = std::min(static_cast<int>(extent.width), std::max(dx0, dx1));
    int bottom = std::max(0, std::min(dy0, dy1));
    int top = std::min(static_cast<int>(extent.height), std::max(dy0, dy1));
    if (overlayScissorEnabled) {
        left = std::max(left, overlayScissorGl[0]);
        bottom = std::max(bottom, overlayScissorGl[1]);
        right = static_cast<int>(std::min<int64_t>(right, static_cast<int64_t>(overlayScissorGl[0]) + overlayScissorGl[2]));
        top = static_cast<int>(std::min<int64_t>(top, static_cast<int64_t>(overlayScissorGl[1]) + overlayScissorGl[3]));
    }
    if (left >= right || bottom >= top) return;
    if (source.image == destination.image) {
        // Keep a snapshot so even disjoint regions never bind one image simultaneously
        // as a sampled source and an attachment. The snapshot belongs to this GPU frame.
        auto saved = source;
        saved.image = vk::DeviceLocalImage::create(frame->device, framework->vma(), false,
            source.extent.width, source.extent.height, 1, source.format,
            VK_IMAGE_USAGE_SAMPLED_BIT | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
        saved.level = 0;
        saved.viewIndex = 0;
        const auto oldLayout = source.image->imageLayout();
        blitTransition(frame->overlayCommandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        blitTransition(frame->overlayCommandBuffer, saved, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region{};
        const VkImageAspectFlags copyAspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource = {copyAspect, source.level, 0, 1};
        region.dstSubresource = {region.srcSubresource.aspectMask, 0, 0, 1};
        region.extent = {source.extent.width, source.extent.height, 1};
        vkCmdCopyImage(frame->overlayCommandBuffer->vkCommandBuffer(), source.image->vkImage(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, saved.image->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        blitTransition(frame->overlayCommandBuffer, source, oldLayout);
        blitAttachment(saved, destination, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, depth, filter);
        return;
    }
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(frame->physicalDevice->vkPhysicalDevice(), source.format, &properties);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        (filter == 0x2601 ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0);
    if ((properties.optimalTilingFeatures & required) != required)
        throw std::runtime_error("Framebuffer blit source does not support requested sampling");
    auto sampler = vk::Sampler::create(frame->device, filter == 0x2601 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                        VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    auto descriptor = vk::DescriptorTableBuilder{}.beginDescriptorLayoutSet()
        .beginDescriptorLayoutSetBinding().defineDescriptorLayoutSetBinding({
            .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        }).endDescriptorLayoutSetBinding().endDescriptorLayoutSet()
        .definePushConstant({VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BlitPush)}).build(frame->device);
    const auto sourceLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const auto targetLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const auto previousSourceLayout = source.image->imageLayout();
    blitTransition(frame->overlayCommandBuffer, source, sourceLayout);
    blitTransition(frame->overlayCommandBuffer, destination, targetLayout);
    // Default view 0 is a depth-only view for combined depth/stencil images, and spans all mips.
    descriptor->bindSamplerImage(sampler, source.image, sourceLayout, 0, 0, 0);
    const std::string key = std::string(depth ? "blit-depth:" : "blit-color:") + std::to_string(destination.format);
    auto &pass = module->framebufferRenderPasses_[key];
    if (!pass) {
        vk::RenderPassBuilder builder;
        builder.beginAttachmentDescription().defineAttachmentDescription({
            .format = destination.format, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = targetLayout, .finalLayout = targetLayout,
        }).endAttachmentDescription().beginAttachmentReference()
            .defineAttachmentReference({0, targetLayout}).endAttachmentReference();
        auto &subpass = builder.beginSubpassDescription();
        if (depth) subpass.defineSubpassDescription({.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .depthStencilAttachmentIndex = 0});
        else subpass.defineSubpassDescription({.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentIndices = {0}});
        subpass.endSubpassDescription();
        pass = builder.build(frame->device);
    }
    auto target = vk::FramebufferBuilder{}.beginAttachment().defineAttachment(destination.image, destination.viewIndex)
        .endAttachment().build(frame->device, pass, extent.width, extent.height);
    auto &pipeline = module->framebufferBlitPipelines_[key];
    if (!pipeline) {
        const auto path = Renderer::folderPath / "shaders/overlay";
        auto vertex = vk::Shader::create(frame->device, (path / "clear_vert.spv").string());
        auto fragment = vk::Shader::create(frame->device,
            (path / (depth ? "framebuffer_depth_blit_frag.spv" : "framebuffer_blit_frag.spv")).string());
        vk::DynamicGraphicsPipelineBuilder builder(depth ? 0 : 1);
        builder.defineRenderPass(pass, 0).beginShaderStage()
            .defineShaderStage(vertex, VK_SHADER_STAGE_VERTEX_BIT).defineShaderStage(fragment, VK_SHADER_STAGE_FRAGMENT_BIT).endShaderStage();
        vk::VertexLayoutInfo empty{};
        builder.defineVertexInputState(empty);
        pipeline = builder.defineInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST).definePipelineLayout(descriptor).build(frame->device);
    }
    frame->overlayCommandBuffer->beginRenderPass({.renderPass = pass, .framebuffer = target, .renderAreaExtent = extent});
    const auto command = frame->overlayCommandBuffer->vkCommandBuffer();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->vkPipeline());
    frame->overlayCommandBuffer->bindDescriptorTable(descriptor, VK_PIPELINE_BIND_POINT_GRAPHICS);
    BlitPush push{{sx0, sy0, sx1, sy1}, {dx0, dy0, dx1, dy1},
        {source.extent.width, source.extent.height}, {extent.width, extent.height}, static_cast<int>(source.level)};
    vkCmdPushConstants(command, descriptor->vkPipelineLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    VkRect2D scissor{{left, static_cast<int>(extent.height) - top}, {static_cast<uint32_t>(right-left), static_cast<uint32_t>(top-bottom)}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdSetCullMode(command, VK_CULL_MODE_NONE);
    vkCmdSetPolygonModeEXT(command, VK_POLYGON_MODE_FILL);
    vkCmdSetDepthBiasEnable(command, VK_FALSE);
    vkCmdSetDepthTestEnable(command, depth);
    vkCmdSetDepthWriteEnable(command, depth);
    vkCmdSetDepthCompareOp(command, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(command, VK_FALSE);
    if (frame->device->hasExtendedDynamicState2LogicOp()) vkCmdSetLogicOpEnableEXT(command, VK_FALSE);
    if (!depth) {
        VkBool32 blend = VK_FALSE;
        VkColorBlendEquationEXT equation{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
                                         VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
        VkColorComponentFlags mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        vkCmdSetColorBlendEnableEXT(command, 0, 1, &blend);
        vkCmdSetColorBlendEquationEXT(command, 0, 1, &equation);
        vkCmdSetColorWriteMaskEXT(command, 0, 1, &mask);
    }
    frame->overlayCommandBuffer->draw(3, 1);
    frame->overlayCommandBuffer->endRenderPass();
    blitTransition(frame->overlayCommandBuffer, source, previousSourceLayout);
    blitTransition(frame->overlayCommandBuffer, destination,
        depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (destination.image == overlayDrawColorImage || destination.image == overlayDrawDepthStencilImage) {
        captureMainAliases();
    }
    framework->frameResourceRetainer().retain(source.image);
    framework->frameResourceRetainer().retain(destination.image);
    framework->frameResourceRetainer().retain(sampler);
    framework->frameResourceRetainer().retain(descriptor);
    framework->frameResourceRetainer().retain(target);
    framework->frameResourceRetainer().retain(pass);
    framework->frameResourceRetainer().retain(pipeline);
}
