#include "core/render/modules/world/temporal_accumulation/temporal_accumulation_module.hpp"

#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"

TemporalAccumulationModule::TemporalAccumulationModule() {}

void TemporalAccumulationModule::init(std::shared_ptr<Framework> framework,
                                      std::shared_ptr<WorldPipeline> worldPipeline) {
    WorldModule::init(framework, worldPipeline);

    uint32_t size = framework->recordingContextCount();

    hdrNoisyImages_.resize(size);
    motionVectorImages_.resize(size);
    normalRoughnessImages_.resize(size);
    accumulatedRadianceOutImages_.resize(size);
    accumulatedNormalOutImages_.resize(size);
}

bool TemporalAccumulationModule::setOrCreateInputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                                        std::vector<VkFormat> &formats,
                                                        uint32_t frameIndex) {
    if (images.size() != inputImageNum) return false;

    auto framework = framework_.lock();
    if (images[0] == nullptr) {
        hdrNoisyImages_[frameIndex] = images[0] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, formats[0],
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    } else {
        if (images[0]->width() != width_ || images[0]->height() != height_) return false;
        hdrNoisyImages_[frameIndex] = images[0];
    }

    if (images[1] == nullptr) {
        motionVectorImages_[frameIndex] = images[1] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, formats[1],
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    } else {
        if (images[1]->width() != width_ || images[1]->height() != height_) return false;
        motionVectorImages_[frameIndex] = images[1];
    }

    if (images[2] == nullptr) {
        normalRoughnessImages_[frameIndex] = images[2] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, formats[2],
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    } else {
        if (images[2]->width() != width_ || images[2]->height() != height_) return false;
        normalRoughnessImages_[frameIndex] = images[2];
    }

    return true;
}

bool TemporalAccumulationModule::setOrCreateOutputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                                         std::vector<VkFormat> &formats,
                                                         uint32_t frameIndex) {
    if (images.size() != 1 || images[0] == nullptr) return false;

    width_ = images[0]->width();
    height_ = images[0]->height();

    accumulatedRadianceOutImages_[frameIndex] = images[0];

    return true;
}

void TemporalAccumulationModule::setAttributes(int attributeCount, std::vector<std::string> &attributeKVs) {}

void TemporalAccumulationModule::build() {
    resetHistoryPending_.assign(worldPipeline_.lock()->viewCount(), 1);
    auto framework = framework_.lock();
    auto worldPipeline = worldPipeline_.lock();
    uint32_t size = framework->recordingContextCount();

    initDescriptorTables();
    initImages();
    initRenderPass();
    initFrameBuffers();
    initPipeline();

    contexts_.resize(size);

    for (int i = 0; i < size; i++) {
        contexts_[i] = TemporalAccumulationModuleContext::create(framework->contexts()[i], worldPipeline->contexts()[i],
                                                                 shared_from_this());
    }
}

std::vector<std::shared_ptr<WorldModuleContext>> &TemporalAccumulationModule::contexts() {
    return contexts_;
}

void TemporalAccumulationModule::bindTexture(std::shared_ptr<vk::Sampler> sampler,
                                             std::shared_ptr<vk::DeviceLocalImage> image,
                                             int index) {}

void TemporalAccumulationModule::onResourceReload() {
    resetHistoryPending_.assign(worldPipeline_.lock()->viewCount(), 1);
}

void TemporalAccumulationModule::preClose() {}

void TemporalAccumulationModule::initDescriptorTables() {
    auto framework = framework_.lock();
    uint32_t size = framework->recordingContextCount();

    descriptorTables_.resize(size);

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
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                   })
                                   .defineDescriptorLayoutSetBinding({
                                       .binding = 2,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                   })
                                   .defineDescriptorLayoutSetBinding({
                                       .binding = 3,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                   })
                                   .defineDescriptorLayoutSetBinding({
                                       .binding = 4,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                   })
                                   .endDescriptorLayoutSetBinding()
                                   .endDescriptorLayoutSet()
                                   .definePushConstant({
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                       .offset = 0,
                                       .size = sizeof(TemporalAccumulationPushConstant),
                                   })
                                   .build(framework->device());
    }

    sampler_ = vk::Sampler::create(framework->device(), VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                   VK_SAMPLER_ADDRESS_MODE_REPEAT);
}

void TemporalAccumulationModule::initImages() {
    auto framework = framework_.lock();
    uint32_t size = framework->recordingContextCount();

    const auto viewCount = worldPipeline_.lock()->viewCount();
    accumulatedRadianceImages_.resize(viewCount);
    accumulatedNormalImages_.resize(viewCount);
    for (uint32_t view = 0; view < viewCount; ++view) {
    accumulatedRadianceImages_[view] = vk::DeviceLocalImage::create(
        framework->device(), framework->vma(), false, hdrNoisyImages_[0]->width(), hdrNoisyImages_[0]->height(), 1,
        hdrNoisyImages_[0]->vkFormat(),
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    accumulatedNormalImages_[view] = vk::DeviceLocalImage::create(
        framework->device(), framework->vma(), false, hdrNoisyImages_[0]->width(), hdrNoisyImages_[0]->height(), 1,
        normalRoughnessImages_[0]->vkFormat(),
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    }
    for (int i = 0; i < size; i++) {
        descriptorTables_[i]->bindSamplerImageForShader(sampler_, hdrNoisyImages_[i], 0, 0);
        descriptorTables_[i]->bindSamplerImageForShader(sampler_, accumulatedRadianceImages_[worldPipeline_.lock()->viewForSlot(i)], 0, 1);
        descriptorTables_[i]->bindSamplerImageForShader(sampler_, motionVectorImages_[i], 0, 2);
        descriptorTables_[i]->bindSamplerImageForShader(sampler_, normalRoughnessImages_[i], 0, 3);
        descriptorTables_[i]->bindSamplerImageForShader(sampler_, accumulatedNormalImages_[worldPipeline_.lock()->viewForSlot(i)], 0, 4);

        accumulatedNormalOutImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, hdrNoisyImages_[0]->width(), hdrNoisyImages_[0]->height(), 1,
            normalRoughnessImages_[0]->vkFormat(),
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
}

void TemporalAccumulationModule::initRenderPass() {
    renderPass_ = vk::RenderPassBuilder{}
                      .beginAttachmentDescription()
                      .defineAttachmentDescription({
                          // color
                          .format = accumulatedRadianceOutImages_[0]->vkFormat(),
                          .samples = VK_SAMPLE_COUNT_1_BIT,
                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                          .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      })
                      .defineAttachmentDescription({
                          // color
                          .format = accumulatedNormalOutImages_[0]->vkFormat(),
                          .samples = VK_SAMPLE_COUNT_1_BIT,
                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                          .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      })
                      .endAttachmentDescription()
                      .beginAttachmentReference()
                      .defineAttachmentReference({
                          .attachment = 0,
                          .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      })
                      .defineAttachmentReference({
                          .attachment = 1,
                          .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      })
                      .endAttachmentReference()
                      .beginSubpassDescription()
                      .defineSubpassDescription({
                          .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                          .colorAttachmentIndices = {0, 1},
                      })
                      .endSubpassDescription()
                      .build(framework_.lock()->device());
}

void TemporalAccumulationModule::initFrameBuffers() {
    auto framework = framework_.lock();
    uint32_t size = framework->recordingContextCount();

    framebuffers_.resize(size);

    for (int i = 0; i < size; i++) {
        framebuffers_[i] = vk::FramebufferBuilder{}
                               .beginAttachment()
                               .defineAttachment(accumulatedRadianceOutImages_[i])
                               .defineAttachment(accumulatedNormalOutImages_[i])
                               .endAttachment()
                               .build(framework->device(), renderPass_);
    }
}

void TemporalAccumulationModule::initPipeline() {
    auto framework = framework_.lock();
    auto worldPipeline = worldPipeline_.lock();
    std::filesystem::path shaderPath = Renderer::folderPath / "shaders";
    vertShader_ =
        vk::Shader::create(framework->device(), (shaderPath / "world/temporal_accumulation/tmp_acc_vert.spv").string());
    fragShader_ =
        vk::Shader::create(framework->device(), (shaderPath / "world/temporal_accumulation/tmp_acc_frag.spv").string());

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
                    .defineDefaultColorBlendAttachmentState() // normal
                    .endColorBlendAttachmentState()
                    .definePipelineLayout(descriptorTables_[0])
                    .build(framework->device());
}

TemporalAccumulationModuleContext::TemporalAccumulationModuleContext(
    std::shared_ptr<FrameworkContext> frameworkContext,
    std::shared_ptr<WorldPipelineContext> worldPipelineContext,
    std::shared_ptr<TemporalAccumulationModule> temporalAccumulationModule)
    : WorldModuleContext(frameworkContext, worldPipelineContext),
      temporalAccumulationModule(temporalAccumulationModule),
      hdrNoisyImage(temporalAccumulationModule->hdrNoisyImages_[frameworkContext->frameIndex]),
      motionVectorImage(temporalAccumulationModule->motionVectorImages_[frameworkContext->frameIndex]),
      normalRoughnessImage(temporalAccumulationModule->normalRoughnessImages_[frameworkContext->frameIndex]),
      descriptorTable(temporalAccumulationModule->descriptorTables_[frameworkContext->frameIndex]),
      framebuffer(temporalAccumulationModule->framebuffers_[frameworkContext->frameIndex]),
      accumulatedRadianceImage(temporalAccumulationModule->accumulatedRadianceImages_[temporalAccumulationModule->worldPipeline_.lock()->viewForSlot(frameworkContext->frameIndex)]),
      accumulatedNormalImage(temporalAccumulationModule->accumulatedNormalImages_[temporalAccumulationModule->worldPipeline_.lock()->viewForSlot(frameworkContext->frameIndex)]),
      accumulatedNormalOutImage(temporalAccumulationModule->accumulatedNormalOutImages_[frameworkContext->frameIndex]),
      accumulatedRadianceOutImage(
          temporalAccumulationModule->accumulatedRadianceOutImages_[frameworkContext->frameIndex]) {}

void TemporalAccumulationModuleContext::render() {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto worldCommandBuffer = context->worldCommandBuffer;
    auto mainQueueIndex = framework->physicalDevice()->mainQueueIndex();

    auto module = temporalAccumulationModule.lock();

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

    VkPipelineStageFlags2 sampledSrcStage = 0;
    VkAccessFlags2 sampledSrcAccess = 0;
    VkPipelineStageFlags2 historySrcStage = 0;
    VkAccessFlags2 historySrcAccess = 0;
    VkPipelineStageFlags2 outputSrcStage = 0;
    VkAccessFlags2 outputSrcAccess = 0;
    VkPipelineStageFlags2 normalOutputSrcStage = 0;
    VkAccessFlags2 normalOutputSrcAccess = 0;

    chooseSrc(hdrNoisyImage->imageLayout(),
              VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, sampledSrcStage, sampledSrcAccess);

    VkPipelineStageFlags2 motionSrcStage = 0;
    VkAccessFlags2 motionSrcAccess = 0;
    chooseSrc(motionVectorImage->imageLayout(),
              VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, motionSrcStage, motionSrcAccess);

    VkPipelineStageFlags2 normalSrcStage = 0;
    VkAccessFlags2 normalSrcAccess = 0;
    chooseSrc(normalRoughnessImage->imageLayout(),
              VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, normalSrcStage, normalSrcAccess);

    chooseSrc(accumulatedRadianceImage->imageLayout(),
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, historySrcStage, historySrcAccess);

    VkPipelineStageFlags2 normalHistorySrcStage = 0;
    VkAccessFlags2 normalHistorySrcAccess = 0;
    chooseSrc(accumulatedNormalImage->imageLayout(),
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, normalHistorySrcStage,
              normalHistorySrcAccess);

    chooseSrc(accumulatedRadianceOutImage->imageLayout(),
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
              outputSrcStage, outputSrcAccess);

    chooseSrc(accumulatedNormalOutImage->imageLayout(),
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
              normalOutputSrcStage, normalOutputSrcAccess);

    TemporalAccumulationPushConstant pc{};
    pc.alpha = module->resetHistoryPending_[module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)] ? 1.0f : module->alpha_;
    pc.threshold = module->threshold_;
    module->resetHistoryPending_[module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)] = false;

    vkCmdPushConstants(worldCommandBuffer->vkCommandBuffer(), descriptorTable->vkPipelineLayout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TemporalAccumulationPushConstant), &pc);

    worldCommandBuffer->barriersBufferImage(
        {}, {{
                 .srcStageMask = sampledSrcStage,
                 .srcAccessMask = sampledSrcAccess,
                 .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .oldLayout = hdrNoisyImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = hdrNoisyImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = motionSrcStage,
                 .srcAccessMask = motionSrcAccess,
                 .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .oldLayout = motionVectorImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = motionVectorImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = normalSrcStage,
                 .srcAccessMask = normalSrcAccess,
                 .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .oldLayout = normalRoughnessImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = normalRoughnessImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = historySrcStage,
                 .srcAccessMask = historySrcAccess,
                 .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .oldLayout = accumulatedRadianceImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = accumulatedRadianceImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = normalHistorySrcStage,
                 .srcAccessMask = normalHistorySrcAccess,
                 .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .oldLayout = accumulatedNormalImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = accumulatedNormalImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = outputSrcStage,
                 .srcAccessMask = outputSrcAccess,
                 .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 .oldLayout = accumulatedRadianceOutImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = accumulatedRadianceOutImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = normalOutputSrcStage,
                 .srcAccessMask = normalOutputSrcAccess,
                 .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 .oldLayout = accumulatedNormalOutImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = accumulatedNormalOutImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             }});
    hdrNoisyImage->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    motionVectorImage->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    normalRoughnessImage->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    accumulatedRadianceImage->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    accumulatedNormalImage->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    accumulatedRadianceOutImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    accumulatedNormalOutImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    worldCommandBuffer->beginRenderPass({
        .renderPass = module->renderPass_,
        .framebuffer = framebuffer,
        .renderAreaExtent = {accumulatedRadianceOutImage->width(), accumulatedRadianceOutImage->height()},
        .clearValues = {{.color = {0.0f, 0.0f, 0.0f, 1.0f}}, {.color = {0.0f, 0.0f, 0.0f, 1.0f}}},
    });

    worldCommandBuffer->bindGraphicsPipeline(module->pipeline_)
        ->bindDescriptorTable(descriptorTable, VK_PIPELINE_BIND_POINT_GRAPHICS)
        ->draw(3, 1)
        ->endRenderPass();
    accumulatedRadianceOutImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    accumulatedNormalOutImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    worldCommandBuffer->barriersBufferImage(
        {}, {{
                 .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                 .oldLayout = accumulatedRadianceOutImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = accumulatedRadianceOutImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                 .oldLayout = accumulatedNormalOutImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = accumulatedNormalOutImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 .oldLayout = accumulatedRadianceImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = accumulatedRadianceImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 .oldLayout = accumulatedNormalImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = accumulatedNormalImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             }});
    accumulatedRadianceOutImage->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    accumulatedNormalOutImage->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    accumulatedRadianceImage->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    accumulatedNormalImage->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    // TODO: add to command buffer
    {
        VkImageBlit imageBlit{};
        imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlit.srcSubresource.mipLevel = 0;
        imageBlit.srcSubresource.baseArrayLayer = 0;
        imageBlit.srcSubresource.layerCount = 1;
        imageBlit.srcOffsets[0] = {0, 0, 0};
        imageBlit.srcOffsets[1] = {static_cast<int>(accumulatedRadianceOutImage->width()),
                                   static_cast<int>(accumulatedRadianceOutImage->height()), 1};
        imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlit.dstSubresource.mipLevel = 0;
        imageBlit.dstSubresource.baseArrayLayer = 0;
        imageBlit.dstSubresource.layerCount = 1;
        imageBlit.dstOffsets[0] = {0, 0, 0};
        imageBlit.dstOffsets[1] = {static_cast<int>(accumulatedRadianceImage->width()),
                                   static_cast<int>(accumulatedRadianceImage->height()), 1};

        vkCmdBlitImage(worldCommandBuffer->vkCommandBuffer(), accumulatedRadianceOutImage->vkImage(),
                       accumulatedRadianceOutImage->imageLayout(), accumulatedRadianceImage->vkImage(),
                       accumulatedRadianceImage->imageLayout(), 1, &imageBlit, VK_FILTER_LINEAR);
    }

    {
        VkImageBlit imageBlit{};
        imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlit.srcSubresource.mipLevel = 0;
        imageBlit.srcSubresource.baseArrayLayer = 0;
        imageBlit.srcSubresource.layerCount = 1;
        imageBlit.srcOffsets[0] = {0, 0, 0};
        imageBlit.srcOffsets[1] = {static_cast<int>(accumulatedNormalOutImage->width()),
                                   static_cast<int>(accumulatedNormalOutImage->height()), 1};
        imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlit.dstSubresource.mipLevel = 0;
        imageBlit.dstSubresource.baseArrayLayer = 0;
        imageBlit.dstSubresource.layerCount = 1;
        imageBlit.dstOffsets[0] = {0, 0, 0};
        imageBlit.dstOffsets[1] = {static_cast<int>(accumulatedNormalImage->width()),
                                   static_cast<int>(accumulatedNormalImage->height()), 1};

        vkCmdBlitImage(worldCommandBuffer->vkCommandBuffer(), accumulatedNormalOutImage->vkImage(),
                       accumulatedNormalOutImage->imageLayout(), accumulatedNormalImage->vkImage(),
                       accumulatedNormalImage->imageLayout(), 1, &imageBlit, VK_FILTER_LINEAR);
    }
}
