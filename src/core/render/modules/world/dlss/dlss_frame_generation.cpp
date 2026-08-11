#include "core/diagnostics/ponder_capture.hpp"
#include "core/logging.hpp"
#include "core/failure_state.hpp"
#include "dlss_frame_generation.hpp"
#include "dlss_module.hpp"
#include "fg_present_policy.hpp"
#include "core/diagnostics/fg_timing.hpp"
#include "core/diagnostics/device_loss_trace.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/buffers.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/ui_blur.hpp"
#include "core/render/modules/world/ray_tracing/ray_tracing_module.hpp"
#include "core/render/streamline_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace {
template<class Image>
void transition(const std::shared_ptr<vk::CommandBuffer> &cmd, const std::shared_ptr<Image> &image,
                VkImageLayout layout) {
    cmd->barriersBufferImage({}, {{
        .srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout=image->imageLayout(), .newLayout=layout,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image, .subresourceRange=vk::wholeColorSubresourceRange}});
    image->imageLayout() = layout;
}
}

DlssFrameGeneration::~DlssFrameGeneration() { mcvr::StreamlineRuntime::get().disableFG(); }

void DlssFrameGeneration::blurHudless(Framework &f, FrameworkContext &frame,
                                    const vk::Data::OverlayPostUBO &parameters,
                                    const std::shared_ptr<vk::DeviceLocalImage> &color) {
    if (!frame.fgHudlessCaptured || frame.frameIndex >= hudless_.size() || !hudless_[frame.frameIndex]) return;
    if (parameters.inSize.x <= 0 || parameters.inSize.y <= 0 ||
        !std::isfinite(parameters.radius * parameters.radiusMultiplier))
        throw std::invalid_argument("Invalid FG background blur parameters");
    if (hudlessBlurScratch_.empty()) {
        hudlessBlurScratch_.resize(f.swapchain()->imageCount());
        hudlessBlurTables_.resize(f.swapchain()->imageCount());
        hudlessBlurWeighted_.resize(f.swapchain()->imageCount());
        hudlessWeightTables_.resize(f.swapchain()->imageCount());
        hudlessBlurSampler_ = vk::Sampler::create(f.device(), VK_FILTER_LINEAR,
            VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    }
    auto source = hudless_[frame.frameIndex];
    auto &scratch = hudlessBlurScratch_[frame.frameIndex];
    auto &table = hudlessBlurTables_[frame.frameIndex];
    auto &weighted = hudlessBlurWeighted_[frame.frameIndex];
    auto &weightTable = hudlessWeightTables_[frame.frameIndex];
    if (!color || color->width() != source->width() || color->height() != source->height())
        throw std::invalid_argument("FG background blur coverage extent mismatch");
    if (!scratch) {
        scratch = vk::DeviceLocalImage::create(f.device(), f.vma(), false, source->width(), source->height(), 1,
            source->vkFormat(), VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        weighted = vk::DeviceLocalImage::create(f.device(), f.vma(), false, source->width(), source->height(), 1,
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        table = vk::DescriptorTableBuilder{}.beginDescriptorLayoutSet().beginDescriptorLayoutSetBinding()
            .defineDescriptorLayoutSetBinding({0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
            .defineDescriptorLayoutSetBinding({1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT})
            .endDescriptorLayoutSetBinding().endDescriptorLayoutSet()
            .definePushConstant({VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(glm::vec4)}).build(f.device());
        table->bindSamplerImage(hudlessBlurSampler_, weighted, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0, 0);
        table->bindImage(scratch, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        weightTable = vk::DescriptorTableBuilder{}.beginDescriptorLayoutSet().beginDescriptorLayoutSetBinding()
            .defineDescriptorLayoutSetBinding({0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
            .defineDescriptorLayoutSetBinding({1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT})
            .defineDescriptorLayoutSetBinding({2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT})
            .endDescriptorLayoutSetBinding().endDescriptorLayoutSet().build(f.device());
        weightTable->bindSamplerImage(hudlessBlurSampler_, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0, 0);
        weightTable->bindSamplerImage(hudlessBlurSampler_, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 2, 0);
        weightTable->bindImage(weighted, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        if (!hudlessWeightPipeline_) {
            auto shader = vk::Shader::create(f.device(), (Renderer::folderPath / "shaders/world/upscaler/fg_hudless_weight_comp.spv").string());
            hudlessWeightPipeline_ = vk::ComputePipelineBuilder{}.defineShader(shader).definePipelineLayout(weightTable).build(f.device());
        }
        if (!hudlessBlurPipeline_) {
            auto shader = vk::Shader::create(f.device(), (Renderer::folderPath / "shaders/world/upscaler/fg_hudless_blur_comp.spv").string());
            hudlessBlurPipeline_ = vk::ComputePipelineBuilder{}.defineShader(shader).definePipelineLayout(table).build(f.device());
        }
    }
    auto cmd = frame.overlayCommandBuffer;
    const auto colorLayout = color->imageLayout();
    transition(cmd, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cmd, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cmd, weighted, VK_IMAGE_LAYOUT_GENERAL);
    cmd->bindDescriptorTable(weightTable, VK_PIPELINE_BIND_POINT_COMPUTE)->bindComputePipeline(hudlessWeightPipeline_);
    vkCmdDispatch(cmd->vkCommandBuffer(), (source->width()+7)/8, (source->height()+7)/8, 1);
    transition(cmd, weighted, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cmd, scratch, VK_IMAGE_LAYOUT_GENERAL);
    const auto push = mcvr::ui::blurParameters(parameters.inSize.x, parameters.inSize.y,
        parameters.blurDir.x, parameters.blurDir.y, parameters.radius, parameters.radiusMultiplier);
    cmd->bindDescriptorTable(table, VK_PIPELINE_BIND_POINT_COMPUTE)->bindComputePipeline(hudlessBlurPipeline_);
    vkCmdPushConstants(cmd->vkCommandBuffer(), table->vkPipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(push), &push);
    vkCmdDispatch(cmd->vkCommandBuffer(), (source->width()+7)/8, (source->height()+7)/8, 1);
    transition(cmd, scratch, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.dstSubresource = copy.srcSubresource;
    copy.extent = {source->width(),source->height(),1};
    vkCmdCopyImage(cmd->vkCommandBuffer(), scratch->vkImage(), scratch->imageLayout(),
        source->vkImage(), source->imageLayout(), 1, &copy);
    transition(cmd, source, VK_IMAGE_LAYOUT_GENERAL);
    transition(cmd, color, colorLayout);
    ++frame.fgWeightedBlurPasses;
}

void DlssFrameGeneration::captureHudless(Framework &f, FrameworkContext &frame,
                                         const std::shared_ptr<vk::DeviceLocalImage> &color) {
    if (!DLSSModule::fgAvailable || failed_) return;
    if (hudless_.empty()) hudless_.resize(f.swapchain()->imageCount());
    auto &image=hudless_[frame.frameIndex];
    if (!image) image=vk::DeviceLocalImage::create(f.device(), f.vma(), false,
        color->width(),color->height(),1,color->vkFormat(),
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    auto cmd=frame.overlayCommandBuffer;
    const auto savedLayout=color->imageLayout();
    // World post-processing owns RGB, not GUI coverage. Establish alpha only at
    // the world/GUI boundary; arbitrary shader packs may leave world alpha at 1.
    if (coverageResetTables_.empty()) coverageResetTables_.resize(f.swapchain()->imageCount());
    auto &resetTable = coverageResetTables_[frame.frameIndex];
    if (!resetTable) {
        resetTable = vk::DescriptorTableBuilder{}.beginDescriptorLayoutSet().beginDescriptorLayoutSetBinding()
            .defineDescriptorLayoutSetBinding({0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT})
            .endDescriptorLayoutSetBinding().endDescriptorLayoutSet().build(f.device());
        resetTable->bindImage(color,VK_IMAGE_LAYOUT_GENERAL,0,0);
        if (!coverageResetPipeline_) {
            auto shader = vk::Shader::create(f.device(),(Renderer::folderPath/"shaders/world/upscaler/dlss_ui_reset_comp.spv").string());
            coverageResetPipeline_ = vk::ComputePipelineBuilder{}.defineShader(shader).definePipelineLayout(resetTable).build(f.device());
        }
    }
    transition(cmd,color,VK_IMAGE_LAYOUT_GENERAL);
    cmd->bindDescriptorTable(resetTable,VK_PIPELINE_BIND_POINT_COMPUTE)->bindComputePipeline(coverageResetPipeline_);
    vkCmdDispatch(cmd->vkCommandBuffer(),(color->width()+15)/16,(color->height()+15)/16,1);
    transition(cmd,color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.dstSubresource=copy.srcSubresource;
    copy.extent={color->width(),color->height(),1};
    vkCmdCopyImage(cmd->vkCommandBuffer(),color->vkImage(),color->imageLayout(),image->vkImage(),image->imageLayout(),1,&copy);
    transition(cmd,color,savedLayout);
    transition(cmd,image,VK_IMAGE_LAYOUT_GENERAL);
    frame.fgHudlessCaptured=true;
}

bool DlssFrameGeneration::initialize(Framework &f, const std::shared_ptr<vk::DeviceLocalImage> &color,
                                    const std::shared_ptr<vk::DeviceLocalImage> &linearDepth) {
    attempted_ = true;
    device_ = f.device();
    auto &sl=mcvr::StreamlineRuntime::get();
    sl::DLSSGState limits{};
    if (!sl.slDLSSGGetState || !sl.check(sl.slDLSSGGetState(sl::ViewportHandle(0),limits,nullptr),"FG limits") ||
        limits.numFramesToGenerateMax<1 || color->width()<limits.minWidthOrHeight || color->height()<limits.minWidthOrHeight ||
        f.swapchain()->presentMode()!=VK_PRESENT_MODE_IMMEDIATE_KHR) {
        mcvr::log::error("DlssFrameGeneration") << "[Streamline] FG disabled: capability, output size or immediate presentation requirement not met" << std::endl;
        return false;
    }
    const auto usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    real_ = vk::DeviceLocalImage::create(device_, f.vma(), false, color->width(), color->height(), 1, color->vkFormat(), usage);

    depth_ = vk::DeviceLocalImage::create(device_, f.vma(), false, linearDepth->width(), linearDepth->height(), 1,
        VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    auto shader = vk::Shader::create(device_, (Renderer::folderPath / "shaders/world/upscaler/dlss_device_depth_comp.spv").string());
    for (uint32_t i=0; i<f.swapchain()->imageCount(); ++i) {
        auto table = vk::DescriptorTableBuilder{}.beginDescriptorLayoutSet().beginDescriptorLayoutSetBinding()
            .defineDescriptorLayoutSetBinding({.binding=0, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT})
            .defineDescriptorLayoutSetBinding({.binding=1, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT})
            .endDescriptorLayoutSetBinding().endDescriptorLayoutSet()
            .definePushConstant({.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT, .offset=0, .size=sizeof(glm::vec4)}).build(device_);
        table->bindImage(depth_, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        depthTables_.push_back(table);
    }
    depthPipeline_ = vk::ComputePipelineBuilder{}.defineShader(shader).definePipelineLayout(depthTables_[0]).build(device_);
    uiAlpha_ = vk::DeviceLocalImage::create(device_, f.vma(), false, color->width(), color->height(), 1,
        VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    auto uiShader = vk::Shader::create(device_, (Renderer::folderPath / "shaders/world/upscaler/dlss_ui_coverage_comp.spv").string());
    for (uint32_t i=0; i<f.swapchain()->imageCount(); ++i) {
        auto table = vk::DescriptorTableBuilder{}.beginDescriptorLayoutSet().beginDescriptorLayoutSetBinding()
            .defineDescriptorLayoutSetBinding({.binding=0, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT})
            .defineDescriptorLayoutSetBinding({.binding=2, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT})
            .endDescriptorLayoutSetBinding().endDescriptorLayoutSet().build(device_);
        table->bindImage(real_, VK_IMAGE_LAYOUT_GENERAL, 0, 0);
        table->bindImage(uiAlpha_, VK_IMAGE_LAYOUT_GENERAL, 0, 2);
        uiTables_.push_back(table);
    }
    uiPipeline_ = vk::ComputePipelineBuilder{}.defineShader(uiShader).definePipelineLayout(uiTables_[0]).build(device_);
    return true;
}

std::shared_ptr<vk::DeviceLocalImage> DlssFrameGeneration::record(Framework &f, FrameworkContext &frame,
                                                               const std::shared_ptr<PipelineContext> &pipeline) {
    mcvr::fgdiag::Scope recordTiming(mcvr::fgdiag::Record);
    generated_ = false;
    if (const auto result = static_cast<VkResult>(asyncError_.exchange(0)); result != VK_SUCCESS) {
        mcvr::dlss::applyAsyncPresentResult(result, failed_, reset_, Renderer::options.presentationChanged);
        mcvr::log::warn("DlssFrameGeneration") << "[Streamline] asynchronous FG VkResult=" << result
            << " failed=" << failed_ << " surfaceCheck=" << Renderer::options.presentationChanged << std::endl;
    }
    auto color = pipeline->uiModuleContext->overlayDrawColorImage;
    if (!Renderer::options.dlssFrameGeneration && frame.worldRendered) {
        // The same explicit capture request works for the FG-off reference.
        // No SDK resources or HUD-less allocation are required for this probe.
        if (auto capture = mcvr::diagnostics::PonderCapture::begin(f.shared_from_this(), Renderer::instance().buffers(), "FG")) {
            capture->copy(f.shared_from_this(), frame.fuseCommandBuffer, "final-color", color);
            if (frame.fgHudlessCaptured)
                capture->copy(f.shared_from_this(), frame.fuseCommandBuffer, "hudless", hudless_[frame.frameIndex]);
            std::ofstream(capture->directory / "contract.txt")
                << "fgRequested=0\nscope=normal RGB reference; no FG inputs are tagged\n"
                << "affineBackgroundDraws=" << frame.fgAffineBackgroundDraws << "\n"
                << "weightedBlurPasses=" << frame.fgWeightedBlurPasses << "\n";
            capture->sealCommands(frame.fuseCommandBuffer);
        }
    }
    if (!Renderer::options.dlssFrameGeneration || !DLSSModule::fgAvailable || !frame.worldRendered || !frame.fgHudlessCaptured || !pipeline->worldPipelineContext) {
        reset_ = true; mcvr::StreamlineRuntime::get().disableFG(); return color;
    }
    std::shared_ptr<RayTracingModuleContext> rt;
    for (auto &module : pipeline->worldPipelineContext->worldModuleContexts)
        if (auto candidate = std::dynamic_pointer_cast<RayTracingModuleContext>(module)) { rt = candidate; break; }
    if (!rt || !rt->linearDepthImage || !rt->motionVectorImage) { reset_ = true; mcvr::StreamlineRuntime::get().disableFG(); return color; }
    if (!attempted_) failed_ = !initialize(f, color, rt->linearDepthImage);
    if (failed_) { mcvr::StreamlineRuntime::get().disableFG(); return color; }
    if (depth_->width()!=rt->linearDepthImage->width() || depth_->height()!=rt->linearDepthImage->height() ||
        real_->width()!=color->width() || real_->height()!=color->height()) { reset_=true; mcvr::StreamlineRuntime::get().disableFG(); return color; }
    auto cmd = frame.fuseCommandBuffer;
    const auto &ubo = *static_cast<vk::Data::WorldUBO *>(Renderer::instance().buffers()->worldUniformBuffer()->mappedPtr());
    auto table = depthTables_[frame.frameIndex];
    table->bindImage(rt->linearDepthImage, VK_IMAGE_LAYOUT_GENERAL, 0, 0);
    transition(cmd, rt->linearDepthImage, VK_IMAGE_LAYOUT_GENERAL);
    transition(cmd, depth_, VK_IMAGE_LAYOUT_GENERAL);
    cmd->bindDescriptorTable(table, VK_PIPELINE_BIND_POINT_COMPUTE)->bindComputePipeline(depthPipeline_);
    const auto &p = ubo.cameraProjMat;
    const glm::vec4 coefficients(p[2][2], p[3][2], p[2][3], p[3][3]);
    vkCmdPushConstants(cmd->vkCommandBuffer(), table->vkPipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(coefficients), &coefficients);
    vkCmdDispatch(cmd->vkCommandBuffer(), (depth_->width()+15)/16, (depth_->height()+15)/16, 1);
    transition(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cmd, real_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.dstSubresource=copy.srcSubresource;
    copy.extent={color->width(),color->height(),1};
    vkCmdCopyImage(cmd->vkCommandBuffer(),color->vkImage(),color->imageLayout(),real_->vkImage(),real_->imageLayout(),1,&copy);
    auto hudless = hudless_[frame.frameIndex];
    transition(cmd, real_, VK_IMAGE_LAYOUT_GENERAL);
    transition(cmd, hudless, VK_IMAGE_LAYOUT_GENERAL);
    transition(cmd, uiAlpha_, VK_IMAGE_LAYOUT_GENERAL);
    auto uiTable = uiTables_[frame.frameIndex];
    cmd->bindDescriptorTable(uiTable, VK_PIPELINE_BIND_POINT_COMPUTE)->bindComputePipeline(uiPipeline_);
    vkCmdDispatch(cmd->vkCommandBuffer(), (real_->width()+15)/16, (real_->height()+15)/16, 1);
    transition(cmd, hudless, VK_IMAGE_LAYOUT_GENERAL);
    transition(cmd, uiAlpha_, VK_IMAGE_LAYOUT_GENERAL);
    transition(cmd, depth_, VK_IMAGE_LAYOUT_GENERAL);
    transition(cmd, rt->motionVectorImage, VK_IMAGE_LAYOUT_GENERAL);
    auto &sl=mcvr::StreamlineRuntime::get();
    auto token=sl.frame();
    if (!token || !sl.slDLSSGSetOptions) return color;
    sl::DLSSGOptions options{};
    options.mode=sl::DLSSGMode::eOn; options.numFramesToGenerate=1;
    options.flags=sl::DLSSGFlags::eEnableFullscreenMenuDetection;
    options.mvecDepthWidth=depth_->width(); options.mvecDepthHeight=depth_->height();
    options.enableUserInterfaceRecomposition=sl::eTrue;
    options.onErrorCallback=[](const sl::APIError &error) {
        const auto result = static_cast<VkResult>(error.vkRes);
        asyncError_.store(static_cast<int>(result), std::memory_order_release);
        if (result == VK_ERROR_DEVICE_LOST) {
            mcvr::failure::record(mcvr::failure::Kind::deviceLost, result,
                                  "Streamline DLSS-G asynchronous callback");
            mcvr::diagnostics::device_loss::dump(
                "Streamline DLSS-G asynchronous callback", result);
        }
    };
    const sl::ViewportHandle viewport(0);
    if (!sl.check(sl.slDLSSGSetOptions(viewport,options),"FG options")) { failed_=true; sl.disableFG(); return color; }
    const glm::dvec3 position(ubo.cameraPos);
    const auto translation=glm::translate(glm::mat4(1),glm::vec3(position-previousPosition_));
    const auto previous=previousProjection_*previousView_*translation*ubo.cameraViewMatInv*ubo.cameraProjMatInv;
    auto constants=mcvr::slConstants({depth_->width(),depth_->height()},ubo.cameraJitter,ubo.cameraViewMat,p,previous,
        reset_ || glm::length(position-previousPosition_)>64.0);
    constants.cameraPos={float(position.x),float(position.y),float(position.z)};
    if (!sl.check(sl.slSetConstants(constants,*token,viewport),"FG constants")) { failed_=true; sl.disableFG(); return color; }
    const std::shared_ptr<vk::DeviceLocalImage> images[]{depth_,rt->motionVectorImage,hudless,uiAlpha_};
    const sl::BufferType types[]{sl::kBufferTypeDepth,sl::kBufferTypeMotionVectors,sl::kBufferTypeHUDLessColor,sl::kBufferTypeUIAlpha};
    std::array<sl::Resource,4> resources{};
    std::vector<sl::ResourceTag> tags;
    for (unsigned i=0;i<4;++i) {
        auto &image=*images[i]; auto &r=resources[i];
        r=sl::Resource(sl::ResourceType::eTex2d,(void*)image.vkImage(),nullptr,(void*)image.vkImageView(),VK_IMAGE_LAYOUT_GENERAL);
        r.width=image.width(); r.height=image.height(); r.nativeFormat=image.vkFormat(); r.mipLevels=1; r.arrayLayers=1; r.flags=0; r.usage=VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        sl::Extent extent{0,0,r.width,r.height};
        tags.emplace_back(&r,types[i],sl::ResourceLifecycle::eValidUntilPresent,&extent);
    }
    if (auto capture = mcvr::diagnostics::PonderCapture::begin(f.shared_from_this(), Renderer::instance().buffers(), "FG")) {
        capture->copy(f.shared_from_this(), cmd, "final-color", color);
        capture->copy(f.shared_from_this(), cmd, "hudless", hudless);
        capture->copy(f.shared_from_this(), cmd, "ui-alpha", uiAlpha_);
        std::ofstream(capture->directory / "contract.txt")
            << "standard=Final=PremultipliedUI+(1-alpha)*Hudless\n"
            << "fgRequested=1\n"
            << "nonSourceOverDraws=" << frame.fgBackgroundDependentDraws << "\n"
            << "affineBackgroundDraws=" << frame.fgAffineBackgroundDraws << "\n"
            << "weightedBlurPasses=" << frame.fgWeightedBlurPasses << "\n"
            << "scope=real-frame inputs; generated pixels and visual quality are not validated by this capture\n";
        capture->sealCommands(cmd);
    }
    if (frame.fgBackgroundDependentDraws && !backgroundLimitReported_) {
        mcvr::log::warn("DlssFrameGeneration") << "UI blend outside source-over and supported affine background replay observed; FG composition requires inspection." << std::endl;
        backgroundLimitReported_ = true;
    }
    const sl::Extent backbufferExtent{0,0,color->width(),color->height()};
    tags.emplace_back(nullptr,sl::kBufferTypeBackbuffer,sl::ResourceLifecycle::eValidUntilPresent,&backbufferExtent);
    if (!sl.check(sl.slSetTagForFrame(*token,viewport,tags.data(),uint32_t(tags.size()),(sl::CommandBuffer*)cmd->vkCommandBuffer()),"FG tags")) {
        failed_=true; sl.disableFG(); return color;
    }
    previousProjection_=p; previousView_=ubo.cameraViewMat; previousPosition_=position;
    generated_=true; reset_=false;
    return color; // Exactly one real present; Streamline owns generated presents.
}
