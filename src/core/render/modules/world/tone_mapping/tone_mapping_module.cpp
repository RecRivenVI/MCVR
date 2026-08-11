#include "core/render/scene_scope.hpp"
#include "core/render/modules/world/tone_mapping/tone_mapping_module.hpp"

#include "core/render/buffers.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace {

bool tryParseFloat(const std::string &text, float &outValue) {
    try {
        outValue = std::stof(text);
        return true;
    } catch (...) { return false; }
}

bool tryParseInt(const std::string &text, int &outValue) {
    try {
        outValue = std::stoi(text);
        return true;
    } catch (...) { return false; }
}

bool parseBoolValue(const std::string &value, bool fallback) {
    if (value == "render_pipeline.true")
        return true;
    else if (value == "render_pipeline.false")
        return false;
    else
        return fallback;
}

int parseToneMappingMethodValue(const std::string &value, int fallback) {
    if (value == "render_pipeline.module.tone_mapping.attribute.method.pbr_neutral") return TONE_MAPPING_METHOD_PBR_NEUTRAL;
    if (value == "render_pipeline.module.tone_mapping.attribute.method.reinhard") return TONE_MAPPING_METHOD_REINHARD;
    if (value == "render_pipeline.module.tone_mapping.attribute.method.reinhard_white_point") return TONE_MAPPING_METHOD_REINHARD_WHITE_POINT;
    if (value == "render_pipeline.module.tone_mapping.attribute.method.aces") return TONE_MAPPING_METHOD_ACES_FITTED;
    if (value == "render_pipeline.module.tone_mapping.attribute.method.aces_white_point")
        return TONE_MAPPING_METHOD_ACES_FITTED_WHITE_POINT;
    if (value == "render_pipeline.module.tone_mapping.attribute.method.uncharted2") return TONE_MAPPING_METHOD_UNCHARTED2;
    return fallback;
}

int parseExposureMeteringModeValue(const std::string &value, int fallback) {
    if (value == "render_pipeline.module.tone_mapping.attribute.exposure_metering_mode.global")
        return TONE_MAPPING_EXPOSURE_METERING_MODE_GLOBAL;
    if (value == "render_pipeline.module.tone_mapping.attribute.exposure_metering_mode.center")
        return TONE_MAPPING_EXPOSURE_METERING_MODE_CENTER;
    return fallback;
}

} // namespace

ToneMappingModule::ToneMappingModule() {}

void ToneMappingModule::init(std::shared_ptr<Framework> framework, std::shared_ptr<WorldPipeline> worldPipeline) {
    WorldModule::init(framework, worldPipeline);

    uint32_t size = framework->recordingContextCount();

    hdrImages_.resize(size);
    ldrImages_.resize(size);
}

bool ToneMappingModule::setOrCreateInputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                               std::vector<VkFormat> &formats,
                                               uint32_t frameIndex) {
    if (images.size() == 0) return false;

    auto framework = framework_.lock();
    if (images[0] == nullptr) {
        hdrImages_[frameIndex] = images[0] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, formats[0],
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    } else {
        if (images[0]->width() != width_ || images[0]->height() != height_) return false;
        hdrImages_[frameIndex] = images[0];
    }

    return true;
}

bool ToneMappingModule::setOrCreateOutputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                                std::vector<VkFormat> &formats,
                                                uint32_t frameIndex) {
    if (images.size() == 0 || images[0] == nullptr) return false;

    width_ = images[0]->width();
    height_ = images[0]->height();

    ldrImages_[frameIndex] = images[0];

    return true;
}

void ToneMappingModule::setAttributes(int attributeCount, std::vector<std::string> &attributeKVs) {
    for (int i = 0; i < attributeCount; i++) {
        const std::string &key = attributeKVs[2 * i];
        const std::string &value = attributeKVs[2 * i + 1];

        float floatValue = 0.0f;
        if (key == "render_pipeline.module.tone_mapping.attribute.middle_grey") {
            if (tryParseFloat(value, floatValue)) middleGrey_ = std::max(floatValue, 1e-4f);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.exposure_up_speed") {
            if (tryParseFloat(value, floatValue)) speedUp_ = std::max(floatValue, 0.0f);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.exposure_down_speed") {
            if (tryParseFloat(value, floatValue)) speedDown_ = std::max(floatValue, 0.0f);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.log2_luminance_min") {
            if (tryParseFloat(value, floatValue)) log2Min_ = floatValue;
        } else if (key == "render_pipeline.module.tone_mapping.attribute.log2_luminance_max") {
            if (tryParseFloat(value, floatValue)) log2Max_ = floatValue;
        } else if (key == "render_pipeline.module.tone_mapping.attribute.histogram_epsilon") {
            if (tryParseFloat(value, floatValue)) epsilon_ = std::max(floatValue, 1e-8f);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.low_percent") {
            if (tryParseFloat(value, floatValue)) lowPercent_ = floatValue;
        } else if (key == "render_pipeline.module.tone_mapping.attribute.high_percent") {
            if (tryParseFloat(value, floatValue)) highPercent_ = floatValue;
        } else if (key == "render_pipeline.module.tone_mapping.attribute.min_exposure") {
            if (tryParseFloat(value, floatValue)) minExposure_ = std::max(floatValue, 1e-6f);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.max_exposure") {
            if (tryParseFloat(value, floatValue)) maxExposure_ = std::max(floatValue, 1e-6f);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.enable_auto_exposure") {
            isAutoExposureEnabled_ = parseBoolValue(value, isAutoExposureEnabled_);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.manual_exposure") {
            if (tryParseFloat(value, floatValue)) manualExposure_ = std::max(floatValue, 1e-6f);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.exposure_bias") {
            if (tryParseFloat(value, floatValue)) exposureBias_ = floatValue;
        } else if (key == "render_pipeline.module.tone_mapping.attribute.white_point") {
            if (tryParseFloat(value, floatValue)) whitePoint_ = std::max(floatValue, 1e-3f);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.saturation") {
            if (tryParseFloat(value, floatValue)) saturation_ = std::max(floatValue, 0.0f);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.clamp_output") {
            shouldClampOutput_ = parseBoolValue(value, shouldClampOutput_);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.method") {
            toneMappingMethod_ = parseToneMappingMethodValue(value, toneMappingMethod_);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.exposure_metering_mode") {
            exposureMeteringMode_ = parseExposureMeteringModeValue(value, exposureMeteringMode_);
        } else if (key == "render_pipeline.module.tone_mapping.attribute.center_metering_percent") {
            if (tryParseFloat(value, floatValue)) centerMeteringPercent_ = std::clamp(floatValue, 1.0f, 100.0f);
        }
    }
}

void ToneMappingModule::build() {
    auto framework = framework_.lock();
    auto worldPipeline = worldPipeline_.lock();
    uint32_t size = framework->recordingContextCount();

    initDescriptorTables();
    initImages();
    initBuffers();
    initRenderPass();
    initFrameBuffers();
    initPipeline();

    contexts_.resize(size);

    for (int i = 0; i < size; i++) {
        contexts_[i] = ToneMappingModuleContext::create(framework->contexts()[i], worldPipeline->contexts()[i],
                                                        shared_from_this());
    }

    lastTimePoint_ = std::chrono::high_resolution_clock::now();
}

std::vector<std::shared_ptr<WorldModuleContext>> &ToneMappingModule::contexts() {
    return contexts_;
}

void ToneMappingModule::bindTexture(std::shared_ptr<vk::Sampler> sampler,
                                    std::shared_ptr<vk::DeviceLocalImage> image,
                                    int index) {
    auto framework = framework_.lock();
    const uint32_t size = framework->recordingContextCount();
    for (uint32_t i = 0; i < size; i++) {
        if (SceneRecordingScope::active() && i != framework->safeAcquireCurrentContext()->frameIndex) continue;
        if (descriptorTables_[i] != nullptr) {
            descriptorTables_[i]->bindSamplerImage(sampler, image,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 3, index);
        }
    }
}

void ToneMappingModule::preClose() {}

std::shared_ptr<WorldModuleRebuildState> ToneMappingModule::captureRebuildState() const {
    auto state = std::make_shared<ExposureRebuildState>();
    state->buffer = exposures_[0].data;
    state->initialized = exposures_[0].initialized;
    state->device = framework_.lock()->device()->vkDevice();
    return state;
}

void ToneMappingModule::restoreRebuildState(const std::shared_ptr<WorldModuleRebuildState> &state) {
    auto previous = std::dynamic_pointer_cast<ExposureRebuildState>(state);
    if (previous && previous->buffer && previous->buffer->size() == sizeof(ToneMappingModuleExposureData) &&
        previous->device == framework_.lock()->device()->vkDevice()) {
        exposures_[0].data = previous->buffer;
        exposures_[0].initialized = previous->initialized;
    }
}

void ToneMappingModule::initDescriptorTables() {
    auto framework = framework_.lock();
    uint32_t size = framework->recordingContextCount();

    descriptorTables_.resize(size);
    samplers_.resize(size);

    for (int i = 0; i < size; i++) {
        descriptorTables_[i] = vk::DescriptorTableBuilder{}
                                   .beginDescriptorLayoutSet() // set 0
                                   .beginDescriptorLayoutSetBinding()
                                   .defineDescriptorLayoutSetBinding({
                                       .binding = 0,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                   })
                                   .defineDescriptorLayoutSetBinding({
                                       .binding = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                   })
                                   .defineDescriptorLayoutSetBinding({
                                       .binding = 2,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                   })
                                   .defineDescriptorLayoutSetBinding({
                                       .binding = 3,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 4096,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                   })
                                   .defineDescriptorLayoutSetBinding({
                                       .binding = 4,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                   })
                                   .endDescriptorLayoutSetBinding()
                                   .endDescriptorLayoutSet()
                                   .definePushConstant(VkPushConstantRange{
                                       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       .offset = 0,
                                       .size = sizeof(ToneMappingModulePushConstant),
                                   })
                                   .build(framework->device());

        samplers_[i] = vk::Sampler::create(framework->device(), VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                           VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }
}

void ToneMappingModule::initImages() {
    auto framework = framework_.lock();
    uint32_t size = framework->recordingContextCount();

    for (int i = 0; i < size; i++) {
        descriptorTables_[i]->bindSamplerImageForShader(samplers_[i], hdrImages_[i], 0, 0);
    }
}

void ToneMappingModule::initBuffers() {
    auto framework = framework_.lock();
    auto vma = framework->vma();
    auto device = framework->device();
    uint32_t size = framework->recordingContextCount();

    histBuffers_.resize(size);

    exposures_.resize(worldPipeline_.lock()->viewCount());
    for (auto &exposure : exposures_) {
        if (!exposure.data) {
            exposure.data = vk::DeviceLocalBuffer::create(vma, device, sizeof(ToneMappingModuleExposureData),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            exposure.initialized = false;
        }
        exposure.firstFrame = true;
    }

    for (int i = 0; i < size; i++) {
        histBuffers_[i] =
            vk::DeviceLocalBuffer::create(vma, device, histSize * sizeof(uint32_t),
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        descriptorTables_[i]->bindBuffer(histBuffers_[i], 0, 1);

        descriptorTables_[i]->bindBuffer(exposures_[worldPipeline_.lock()->viewForSlot(i)].data, 0, 2);
    }
}

void ToneMappingModule::initRenderPass() {
    renderPass_ = vk::RenderPassBuilder{}
                      .beginAttachmentDescription()
                      .defineAttachmentDescription({
                          // color
                          .format = ldrImages_[0]->vkFormat(),
                          .samples = VK_SAMPLE_COUNT_1_BIT,
                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
#ifdef USE_AMD
                          .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
#else
                          .initialLayout = (SceneRecordingScope::active() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                          .finalLayout = (SceneRecordingScope::active() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
#endif
                      })
                      .endAttachmentDescription()
                      .beginAttachmentReference()
                      .defineAttachmentReference({
                          .attachment = 0,
                          .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      })
                      .endAttachmentReference()
                      .beginSubpassDescription()
                      .defineSubpassDescription({
                          .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                          .colorAttachmentIndices = {0},
                      })
                      .endSubpassDescription()
                      .build(framework_.lock()->device());
}

void ToneMappingModule::initFrameBuffers() {
    auto framework = framework_.lock();
    uint32_t size = framework->recordingContextCount();

    framebuffers_.resize(size);

    for (int i = 0; i < size; i++) {
        framebuffers_[i] = vk::FramebufferBuilder{}
                               .beginAttachment()
                               .defineAttachment(ldrImages_[i])
                               .endAttachment()
                               .build(framework->device(), renderPass_);
    }
}

void ToneMappingModule::initPipeline() {
    auto framework = framework_.lock();
    auto worldPipeline = worldPipeline_.lock();
    auto device = framework->device();
    std::filesystem::path shaderPath = Renderer::folderPath / "shaders";

    histShader_ = vk::Shader::create(framework->device(), (shaderPath / "world/tone_mapping/hist_comp.spv").string());
    histPipeline_ =
        vk::ComputePipelineBuilder{}.defineShader(histShader_).definePipelineLayout(descriptorTables_[0]).build(device);

    exposureShader_ =
        vk::Shader::create(framework->device(), (shaderPath / "world/tone_mapping/exposure_comp.spv").string());
    exposurePipeline_ = vk::ComputePipelineBuilder{}
                            .defineShader(exposureShader_)
                            .definePipelineLayout(descriptorTables_[0])
                            .build(device);

    vertShader_ =
        vk::Shader::create(framework->device(), (shaderPath / "world/tone_mapping/tone_mapping_vert.spv").string());
    fragShader_ =
        vk::Shader::create(framework->device(), (shaderPath / "world/tone_mapping/tone_mapping_frag.spv").string());

    pipeline_ = vk::GraphicsPipelineBuilder{}
                    .defineRenderPass(renderPass_, 0)
                    .beginShaderStage()
                    .defineShaderStage(vertShader_, VK_SHADER_STAGE_VERTEX_BIT)
                    .defineShaderStage(fragShader_, VK_SHADER_STAGE_FRAGMENT_BIT)
                    .endShaderStage()
                    .defineVertexInputState<void>()
                    .defineViewportScissorState({
                        .viewport =
                            {
                                .x = 0,
                                .y = 0,
                                .width = static_cast<float>(worldPipeline->renderExtent().width),
                                .height = static_cast<float>(worldPipeline->renderExtent().height),
                                .minDepth = 0.0,
                                .maxDepth = 1.0,
                            },
                        .scissor =
                            {
                                .offset = {.x = 0, .y = 0},
                                .extent = worldPipeline->renderExtent(),
                            },
                    })
                    .defineDepthStencilState({
                        .depthTestEnable = VK_FALSE,
                        .depthWriteEnable = VK_FALSE,
                        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
                        .depthBoundsTestEnable = VK_FALSE,
                        .stencilTestEnable = VK_FALSE,
                    })
                    .beginColorBlendAttachmentState()
                    .defineDefaultColorBlendAttachmentState() // color
                    .endColorBlendAttachmentState()
                    .definePipelineLayout(descriptorTables_[0])
                    .build(device);
}

ToneMappingModuleContext::ToneMappingModuleContext(std::shared_ptr<FrameworkContext> frameworkContext,
                                                   std::shared_ptr<WorldPipelineContext> worldPipelineContext,
                                                   std::shared_ptr<ToneMappingModule> toneMappingModule)
    : WorldModuleContext(frameworkContext, worldPipelineContext),
      toneMappingModule(toneMappingModule),
      hdrImage(toneMappingModule->hdrImages_[frameworkContext->frameIndex]),
      descriptorTable(toneMappingModule->descriptorTables_[frameworkContext->frameIndex]),
      framebuffer(toneMappingModule->framebuffers_[frameworkContext->frameIndex]),
      histBuffer(toneMappingModule->histBuffers_[frameworkContext->frameIndex]),
      ldrImage(toneMappingModule->ldrImages_[frameworkContext->frameIndex]) {}

void ToneMappingModuleContext::render() {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto worldCommandBuffer = context->worldCommandBuffer;
    auto mainQueueIndex = framework->physicalDevice()->mainQueueIndex();

    auto module = toneMappingModule.lock();
    auto buffers = Renderer::instance().buffers();
    descriptorTable->bindBuffer(buffers->worldUniformBuffer(), 0, 4);

    const bool initializeExposure = !module->exposures_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).initialized;
    if (initializeExposure) {
        vkCmdFillBuffer(worldCommandBuffer->vkCommandBuffer(), module->exposures_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).data->vkBuffer(),
                        0, sizeof(ToneMappingModuleExposureData), 0);
        module->exposures_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).initialized = true;
    }
    worldCommandBuffer->barriersBufferImage({{
        .srcStageMask = initializeExposure ? VK_PIPELINE_STAGE_2_TRANSFER_BIT :
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = initializeExposure ? VK_ACCESS_2_TRANSFER_WRITE_BIT :
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = mainQueueIndex,
        .dstQueueFamilyIndex = mainQueueIndex,
        .buffer = module->exposures_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).data,
    }}, {});

    auto chooseSrc = [](VkImageLayout oldLayout, VkPipelineStageFlags2 fallbackStage, VkAccessFlags2 fallbackAccess,
                        VkPipelineStageFlags2 &outStage, VkAccessFlags2 &outAccess) {
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            outStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            outAccess = 0;
        } else {
            outStage = fallbackStage;
            outAccess = fallbackAccess;
        }
    };

    VkPipelineStageFlags2 hdrSrcStage = 0;
    VkAccessFlags2 hdrSrcAccess = 0;
    chooseSrc(hdrImage->imageLayout(),
              VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, hdrSrcStage, hdrSrcAccess);

    VkPipelineStageFlags2 ldrSrcStage = 0;
    VkAccessFlags2 ldrSrcAccess = 0;
    chooseSrc(ldrImage->imageLayout(),
              VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, ldrSrcStage, ldrSrcAccess);

    worldCommandBuffer->barriersBufferImage(
        {{
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .buffer = histBuffer,
        }},
        {{
             .srcStageMask = hdrSrcStage,
             .srcAccessMask = hdrSrcAccess,
             .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_TRANSFER_BIT,
             .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
             .oldLayout = hdrImage->imageLayout(),
             .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             .srcQueueFamilyIndex = mainQueueIndex,
             .dstQueueFamilyIndex = mainQueueIndex,
             .image = hdrImage,
             .subresourceRange = vk::wholeColorSubresourceRange,
         },
         {
             .srcStageMask = ldrSrcStage,
             .srcAccessMask = ldrSrcAccess,
             .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
             .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
             .oldLayout = ldrImage->imageLayout(),
#ifdef USE_AMD
             .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
#else
             .newLayout = (SceneRecordingScope::active() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
#endif
             .srcQueueFamilyIndex = mainQueueIndex,
             .dstQueueFamilyIndex = mainQueueIndex,
             .image = ldrImage,
             .subresourceRange = vk::wholeColorSubresourceRange,
         }});
    hdrImage->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
#ifdef USE_AMD
    ldrImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
#else
    ldrImage->imageLayout() = (SceneRecordingScope::active() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
#endif

    vkCmdFillBuffer(worldCommandBuffer->vkCommandBuffer(), histBuffer->vkBuffer(), 0, VK_WHOLE_SIZE, 0);

    worldCommandBuffer->barriersBufferImage(
        {{
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .buffer = histBuffer,
        }},
        {});

    std::chrono::time_point<std::chrono::high_resolution_clock> currentTimePoint =
        std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsedTime = currentTimePoint - module->lastTimePoint_;
    module->lastTimePoint_ = currentTimePoint;

    float dtSeconds = static_cast<float>(elapsedTime.count());
    if (!std::isfinite(dtSeconds)) dtSeconds = 1.0f / 60.0f;
    dtSeconds = std::clamp(dtSeconds, 0.0f, 1.0f);
    // Rebuild time is not elapsed scene time. Keep inherited exposure on the first
    // frame while newly reset denoisers/history images produce their initial result.
    if (module->exposures_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).firstFrame) dtSeconds = 0.0f;
    module->exposures_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).firstFrame = false;

    float sanitizedLog2Min = std::min(module->log2Min_, module->log2Max_ - 1e-3f);
    float sanitizedLog2Max = std::max(module->log2Max_, sanitizedLog2Min + 1e-3f);
    float sanitizedLowPercent = std::clamp(module->lowPercent_, 0.0f, 0.9999f);
    float sanitizedHighPercent = std::clamp(module->highPercent_, sanitizedLowPercent + 1e-4f, 1.0f);
    float sanitizedCenterMeteringPercent = std::clamp(module->centerMeteringPercent_, 1.0f, 100.0f) / 100.0f;

    ToneMappingModulePushConstant pc{};
    pc.log2Min = sanitizedLog2Min;
    pc.log2Max = sanitizedLog2Max;
    pc.epsilon = std::max(module->epsilon_, 1e-8f);
    pc.lowPercent = sanitizedLowPercent;
    pc.highPercent = sanitizedHighPercent;
    pc.middleGrey = std::max(module->middleGrey_, 1e-4f);
    pc.dt = dtSeconds;
    pc.speedUp = std::max(module->speedUp_, 0.0f);
    pc.speedDown = std::max(module->speedDown_, 0.0f);
    pc.minExposure = std::max(module->minExposure_, 1e-6f);
    pc.maxExposure = std::max(module->maxExposure_, pc.minExposure);
    pc.manualExposure = std::max(module->manualExposure_, 1e-6f);
    pc.exposureBias = module->exposureBias_;
    pc.whitePoint = std::max(module->whitePoint_, 1e-3f);
    pc.saturation = std::max(module->saturation_, 0.0f);
    pc.toneMappingMethod = std::clamp(module->toneMappingMethod_, static_cast<int>(TONE_MAPPING_METHOD_PBR_NEUTRAL),
                                      static_cast<int>(TONE_MAPPING_METHOD_UNCHARTED2));
    pc.autoExposure = module->isAutoExposureEnabled_ ? 1 : 0;
    pc.clampOutput = module->shouldClampOutput_ ? 1 : 0;
    pc.exposureMeteringMode =
        std::clamp(module->exposureMeteringMode_, static_cast<int>(TONE_MAPPING_EXPOSURE_METERING_MODE_GLOBAL),
                   static_cast<int>(TONE_MAPPING_EXPOSURE_METERING_MODE_CENTER));
    pc.centerMeteringPercent = sanitizedCenterMeteringPercent;

    vkCmdPushConstants(worldCommandBuffer->vkCommandBuffer(), descriptorTable->vkPipelineLayout(),
                       VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ToneMappingModulePushConstant), &pc);

    worldCommandBuffer->bindDescriptorTable(descriptorTable, VK_PIPELINE_BIND_POINT_COMPUTE)
        ->bindComputePipeline(module->histPipeline_);

    uint32_t groupX = (module->width_ + 16 - 1) / 16;
    uint32_t groupY = (module->height_ + 16 - 1) / 16;
    vkCmdDispatch(worldCommandBuffer->vkCommandBuffer(), groupX, groupY, 1);

    worldCommandBuffer->barriersBufferImage(
        {{
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .buffer = histBuffer,
        }},
        {});

    worldCommandBuffer->bindComputePipeline(module->exposurePipeline_);
    vkCmdDispatch(worldCommandBuffer->vkCommandBuffer(), 1, 1, 1);

    worldCommandBuffer->barriersBufferImage(
        {{
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .buffer = module->exposures_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).data,
        }},
        {});

    worldCommandBuffer->beginRenderPass({
        .renderPass = module->renderPass_,
        .framebuffer = framebuffer,
        .renderAreaExtent = {ldrImage->width(), ldrImage->height()},
        .clearValues = {{.color = {0.1f, 0.1f, 0.1f, 1.0f}}},
    });
    ldrImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    worldCommandBuffer->bindGraphicsPipeline(module->pipeline_)
        ->bindDescriptorTable(descriptorTable, VK_PIPELINE_BIND_POINT_GRAPHICS)
        ->draw(3, 1)
        ->endRenderPass();
#ifdef USE_AMD
    ldrImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
#else
    ldrImage->imageLayout() = (SceneRecordingScope::active() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
#endif
}
