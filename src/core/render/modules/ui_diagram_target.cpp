#include "core/render/modules/ui_module.hpp"
#include "core/render/buffers.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"

#include <stdexcept>

namespace {
void sampleDiagramImage(const std::shared_ptr<vk::CommandBuffer> &commands,
                        const Framebuffers::ResolvedAttachment &attachment, VkImageLayout layout) {
    commands->barriersBufferImage({}, {{
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = attachment.image->imageLayout(), .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = attachment.image,
        .subresourceRange = {mcvr::framebuffer::formatAspects(attachment.format), 0, VK_REMAINING_MIP_LEVELS, 0, 1},
    }});
    attachment.image->imageLayout() = layout;
}
}

void UIModuleContext::beginDiagramTarget(uint32_t framebuffer, int width, int height) {
    if (framebuffer == 0 || width <= 0 || height <= 0)
        throw std::invalid_argument("Diagram requires a real framebuffer and positive dimensions");
    auto registry = Renderer::instance().framebuffers();
    auto state = std::make_shared<UIModuleContext>(*this);
    const auto read = registry->readFramebufferBinding(), draw = registry->drawFramebufferBinding();
    end();
    try {
        registry->bindFramebuffer(mcvr::framebuffer::FRAMEBUFFER, framebuffer);
        auto source = framebufferSnapshot(mcvr::framebuffer::DRAW_FRAMEBUFFER);
        if (source.status != mcvr::framebuffer::FRAMEBUFFER_COMPLETE || !source.readColor || !source.depthStencil ||
            !(source.depthStencil->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
            source.extent.width != width || source.extent.height != height)
            throw std::runtime_error("Diagram framebuffer must have matching real color/depth attachments");
        diagramTargetStack.push_back({state, source, read, draw});
        overlayDepthTestEnable = true;
        overlayDepthWriteEnable = true;
        overlayDepthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        overlayColorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        overlayClearColors = {0, 0, 0, 0};
        overlayClearDepth = 1.0f;
        setOverlayScissorEnabled(false);
        setOverlayViewport(0, 0, width, height);
        clearMaskedFramebuffer(true, true, false);
    } catch (...) {
        if (!diagramTargetStack.empty() && diagramTargetStack.back().previousState == state) abortDiagramTarget();
        else {
            registry->bindFramebuffer(mcvr::framebuffer::READ_FRAMEBUFFER, read);
            registry->bindFramebuffer(mcvr::framebuffer::DRAW_FRAMEBUFFER, draw);
            copyPersistentStateFrom(*state);
        }
        throw;
    }
}

void UIModuleContext::abortDiagramTarget() {
    if (diagramTargetStack.empty()) return;
    end();
    auto state = std::move(diagramTargetStack.back());
    diagramTargetStack.pop_back();
    auto registry = Renderer::instance().framebuffers();
    registry->bindFramebuffer(mcvr::framebuffer::READ_FRAMEBUFFER, state.readFramebuffer);
    registry->bindFramebuffer(mcvr::framebuffer::DRAW_FRAMEBUFFER, state.drawFramebuffer);
    copyPersistentStateFrom(*state.previousState);
    // Do not begin an enclosing pass until its next draw; bind/scissor state is preserved.
}

void UIModuleContext::postDiagramTarget(uint32_t framebuffer) {
    if (diagramTargetStack.empty()) throw std::logic_error("No active diagram framebuffer");
    if (framebuffer == 0) throw std::invalid_argument("Diagram output must be a real framebuffer");
    try {
        end();
        const auto source = diagramTargetStack.back().source;
        auto frame = frameworkContext.lock();
        auto framework = frame->framework.lock();
        auto module = uiModule.lock();
        auto registry = Renderer::instance().framebuffers();
        registry->bindFramebuffer(mcvr::framebuffer::DRAW_FRAMEBUFFER, framebuffer);
        const auto target = framebufferSnapshot(mcvr::framebuffer::DRAW_FRAMEBUFFER);
        if (target.status != mcvr::framebuffer::FRAMEBUFFER_COMPLETE || target.drawColors.size() != 1 || !target.drawColors[0])
            throw std::runtime_error("Diagram output must have one active color attachment");
        if (source.readColor->image == target.drawColors[0]->image || source.depthStencil->image == target.drawColors[0]->image)
            throw std::runtime_error("Diagram input and output must use separate attachments");
        sampleDiagramImage(frame->overlayCommandBuffer, *source.readColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        sampleDiagramImage(frame->overlayCommandBuffer, *source.depthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        setOverlayScissorEnabled(false);
        setOverlayViewport(0, 0, target.extent.width, target.extent.height);
        overlayDepthTestEnable = false;
        overlayDepthWriteEnable = false;
        overlayStencilTestEnable = false;
        overlayBlendEnabled = false;
        overlayColorLogicOpEnable = false;
        overlayCullMode = VK_CULL_MODE_NONE;
        overlayPolygonMode = VK_POLYGON_MODE_FILL;
        overlayDepthBiasEnable = false;
        overlayColorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        switchFramebufferDraw();
        auto descriptor = module->createOverlayDescriptorTable();
        {
            std::lock_guard lock(module->overlayDescriptorMutex_);
            module->bindOverlayDescriptorTableResources(descriptor, frame->frameIndex);
        }
        auto colorSampler = vk::Sampler::create(frame->device, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        auto depthSampler = vk::Sampler::create(frame->device, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        descriptor->bindSamplerImage(colorSampler, source.readColor->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 2, 0);
        descriptor->bindSamplerImage(depthSampler, source.depthStencil->image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 0, 3, 0);
        const auto key = "diagram-target:" + activeFramebufferPassKey;
        auto &pipeline = module->framebufferBlitPipelines_[key];
        if (!pipeline) {
            auto path = Renderer::folderPath / "shaders/overlay/post";
            auto vertex = vk::Shader::create(frame->device, (path / "diagram_vert.spv").string());
            auto fragment = vk::Shader::create(frame->device, (path / "diagram_target_frag.spv").string());
            vk::DynamicGraphicsPipelineBuilder builder(1);
            builder.defineRenderPass(module->framebufferRenderPasses_.at(activeFramebufferPassKey), 0)
                .beginShaderStage().defineShaderStage(vertex, VK_SHADER_STAGE_VERTEX_BIT)
                .defineShaderStage(fragment, VK_SHADER_STAGE_FRAGMENT_BIT).endShaderStage();
            vk::VertexLayoutInfo empty{};
            builder.defineVertexInputState(empty);
            pipeline = builder.defineInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                .definePipelineLayout(descriptor).build(frame->device);
        }
        const auto post = Renderer::instance().buffers()->getPostID();
        if (post < 0) throw std::runtime_error("Diagram uniform has not been uploaded");
        const auto command = frame->overlayCommandBuffer->vkCommandBuffer();
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->vkPipeline());
        uint32_t offsets[]{0, Renderer::instance().buffers()->overlayPostUniformOffset(post)};
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, descriptor->vkPipelineLayout(), 0,
            descriptor->descriptorSet().size(), descriptor->descriptorSet().data(), 2, offsets);
        frame->overlayCommandBuffer->draw(3, 1);
        auto &retainer = framework->frameResourceRetainer();
        retainer.retain(source.readColor->image); retainer.retain(source.depthStencil->image);
        retainer.retain(descriptor); retainer.retain(colorSampler); retainer.retain(depthSampler); retainer.retain(pipeline);
    } catch (...) {
        // The Java scope owns exceptional cleanup. Popping here as well would
        // make its finally block abort the enclosing diagram on nested failure.
        throw;
    }
    abortDiagramTarget();
}
