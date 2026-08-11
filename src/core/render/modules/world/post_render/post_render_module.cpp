#include "core/logging.hpp"
#include "core/render/scene_scope.hpp"
#include "core/render/post_color_sync.hpp"
#include "core/render/modules/world/post_render/post_render_module.hpp"

#include "core/render/buffers.hpp"
#include "core/render/entities.hpp"
#include "core/render/modules/world/shader_pack/shader_pack.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/util/parallel.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

using json = nlohmann::json;

namespace {

const RenderPass::ShaderVariant *findShaderVariant(const RenderPass &pass, std::string_view contentName) {
    auto variantIter = pass.shaderVariants.find(std::string(contentName));
    if (variantIter != pass.shaderVariants.end()) { return &variantIter->second; }

    variantIter = pass.shaderVariants.find("default");
    if (variantIter != pass.shaderVariants.end()) { return &variantIter->second; }

    return nullptr;
}

std::string findShaderVariantName(const RenderPass &pass, std::string_view contentName) {
    auto variantIter = pass.shaderVariants.find(std::string(contentName));
    if (variantIter != pass.shaderVariants.end()) { return variantIter->first; }

    variantIter = pass.shaderVariants.find("default");
    if (variantIter != pass.shaderVariants.end()) { return variantIter->first; }

    return "";
}

// void logShaderVariantSelectionOnce(const std::string &passName,
//                                    std::string_view contentName,
//                                    std::string_view variantName) {
//     static std::mutex mutex;
//     static std::set<std::string> loggedKeys;

//     const std::string key = passName + "|" + std::string(contentName) + "|" + std::string(variantName);

//     std::lock_guard<std::mutex> lock(mutex);
//     if (!loggedKeys.insert(key).second) { return; }

//     mcvr::log::error("PostRenderModule") << "[PostVariant] pass=" << passName << " content=" << contentName << " variant=" << variantName
//               << std::endl;
// }

} // namespace

PostRenderModule::PostRenderModule() {}

void PostRenderModule::init(std::shared_ptr<Framework> framework, std::shared_ptr<WorldPipeline> worldPipeline) {
    WorldModule::init(framework, worldPipeline);

    uint32_t size = framework->recordingContextCount();

    ldrImages_.resize(size);
    firstHitDepthImages_.resize(size);
    hdrImages_.resize(size);
    motionVectorImages_.resize(size);
    normalRoughnessImages_.resize(size);
    postRenderedImages_.resize(size);
    postRenderedInitialized_.assign(size, 0);
}

bool PostRenderModule::setOrCreateInputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                              std::vector<VkFormat> &formats,
                                              uint32_t frameIndex) {
    if (images.size() != inputImageNum) return false;

    auto framework = framework_.lock();
    if (images[0] == nullptr) {
        ldrImages_[frameIndex] = images[0] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, formats[0],
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    } else {
        if (images[0]->width() != width_ || images[0]->height() != height_) return false;
        ldrImages_[frameIndex] = images[0];
    }

    if (images[1] == nullptr) {
        firstHitDepthImages_[frameIndex] = images[1] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, formats[1],
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    } else {
        if (images[1]->width() != width_ || images[1]->height() != height_) return false;
        firstHitDepthImages_[frameIndex] = images[1];
    }

    if (images[2] == nullptr) {
        hdrImages_[frameIndex] = images[2] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, formats[2],
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    } else {
        if (images[2]->width() != width_ || images[2]->height() != height_) return false;
        hdrImages_[frameIndex] = images[2];
    }

    if (images[3] == nullptr) {
        motionVectorImages_[frameIndex] = images[3] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, formats[3],
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    } else {
        if (images[3]->width() != width_ || images[3]->height() != height_) return false;
        motionVectorImages_[frameIndex] = images[3];
    }

    if (images[4] == nullptr) {
        normalRoughnessImages_[frameIndex] = images[4] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, formats[4],
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    } else {
        if (images[4]->width() != width_ || images[4]->height() != height_) return false;
        normalRoughnessImages_[frameIndex] = images[4];
    }

    return true;
}

bool PostRenderModule::setOrCreateOutputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                               std::vector<VkFormat> &formats,
                                               uint32_t frameIndex) {
    if (images.size() != outputImageNum || images[0] == nullptr) return false;

    width_ = images[0]->width();
    height_ = images[0]->height();

    postRenderedImages_[frameIndex] = images[0];

    return true;
}

void PostRenderModule::setAttributes(int attributeCount, std::vector<std::string> &attributeKVs) {
    (void)attributeCount;
    (void)attributeKVs;
}

void PostRenderModule::initExecutionVariables() {
    executionVariableConfigs_.clear();
    globalVariables_.clear();

    if (shaderPack_ == nullptr) { return; }
    shaderPack_->copyStageExecutionState(ShaderPackLoader::Stage::PostRender, executionVariableConfigs_,
                                         globalVariables_);
}

void PostRenderModule::build() {
#ifdef DEBUG
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;
    auto t0 = clock::now();
    auto printStep = [&t0](const char *label) {
        auto now = clock::now();
        mcvr::log::debug("PostRenderModule") << "Build " << label << ": " << ms(now - t0).count() << " ms" << std::endl;
        t0 = now;
    };

    mcvr::log::debug("PostRenderModule") << "Build started" << std::endl;
#endif

    auto framework = framework_.lock();
    auto worldPipeline = worldPipeline_.lock();
    uint32_t size = framework->recordingContextCount();
#ifdef DEBUG
    printStep("lock framework");
#endif

    shaderPack_ = worldPipeline != nullptr ? worldPipeline->shaderPack() : nullptr;
#ifdef DEBUG
    printStep("loadShaderPack");
#endif

    initExecutionVariables();
#ifdef DEBUG
    printStep("initExecutionVariables");
#endif

    initDescriptorTables();
#ifdef DEBUG
    printStep("initDescriptorTables");
#endif

    initImages();
#ifdef DEBUG
    printStep("initImages");
#endif

    initBuffers();
#ifdef DEBUG
    printStep("initBuffers");
#endif

    initRenderPass();
#ifdef DEBUG
    printStep("initRenderPass");
#endif

    initFrameBuffers();
#ifdef DEBUG
    printStep("initFrameBuffers");
#endif

    initPipeline();
#ifdef DEBUG
    printStep("initPipeline");
#endif

    contexts_.resize(size);

    for (int i = 0; i < size; i++) {
        contexts_[i] =
            PostRenderModuleContext::create(framework->contexts()[i], worldPipeline->contexts()[i], shared_from_this());
    }
#ifdef DEBUG
    printStep("initContexts");
#endif

    ensureDynamicPipelines();
#ifdef DEBUG
    printStep("ensureDynamicPipelines");

    mcvr::log::debug("PostRenderModule") << "Build completed" << std::endl;
#endif
}

std::vector<std::shared_ptr<WorldModuleContext>> &PostRenderModule::contexts() {
    return contexts_;
}

void PostRenderModule::bindTexture(std::shared_ptr<vk::Sampler> sampler,
                                   std::shared_ptr<vk::DeviceLocalImage> image,
                                   int index) {
    auto framework = framework_.lock();

    uint32_t size = framework->recordingContextCount();
    for (int i = 0; i < size; i++) {
        if (SceneRecordingScope::active() && i != framework->safeAcquireCurrentContext()->frameIndex) continue;
        if (descriptorTables_[i] != nullptr)
            descriptorTables_[i]->bindSamplerImage(sampler, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0,
                                                   index);
    }
}

void PostRenderModule::preClose() {
    fullScreenPasses_.clear();
    passNameToPass_.clear();
    renderPasses_.clear();
    renderPassNameToPass_.clear();
    executionVariableConfigs_.clear();
    globalVariables_.clear();
    shaderPack_ = nullptr;
    isDynamicPipelinesReady_ = false;
}

RenderPass::Target PostRenderModule::parseRenderContent(const std::string &content) {
    const std::string lower = ShaderPackLoader::toLower(content);
    if (lower == "weather") { return RenderPass::Target::Weather; }
    if (lower == "particle" || lower == "particles") { return RenderPass::Target::Particle; }
    if (lower == "text") { return RenderPass::Target::Text; }
    throw std::runtime_error("unsupported post_render render content: " + content);
}

int PostRenderModule::renderTargetPostFlag(RenderPass::Target target) {
    switch (target) {
        case RenderPass::Target::Weather:
            return weatherPostFlag;
        case RenderPass::Target::Particle:
            return particlePostFlag;
        case RenderPass::Target::Text:
            return textPostFlag;
    }
    return 0;
}

bool PostRenderModule::renderTargetDefaultDepthWrite(RenderPass::Target target) {
    return target == RenderPass::Target::Text;
}

VkCompareOp PostRenderModule::parseDepthCompare(const std::string &value) {
    const std::string lower = ShaderPackLoader::toLower(value);
    if (lower == "never") { return VK_COMPARE_OP_NEVER; }
    if (lower == "less") { return VK_COMPARE_OP_LESS; }
    if (lower == "equal") { return VK_COMPARE_OP_EQUAL; }
    if (lower == "less_or_equal" || lower == "lequal") { return VK_COMPARE_OP_LESS_OR_EQUAL; }
    if (lower == "greater") { return VK_COMPARE_OP_GREATER; }
    if (lower == "not_equal") { return VK_COMPARE_OP_NOT_EQUAL; }
    if (lower == "greater_or_equal" || lower == "gequal") { return VK_COMPARE_OP_GREATER_OR_EQUAL; }
    if (lower == "always") { return VK_COMPARE_OP_ALWAYS; }
    throw std::runtime_error("unsupported post_render depth_compare: " + value);
}

void PostRenderModule::initDescriptorTables() {
    auto framework = framework_.lock();
    uint32_t size = framework->recordingContextCount();
    descriptorTables_.resize(size);
    samplers_.resize(size);
    postPassColorSamplers_.resize(size);
    postPassDepthSamplers_.resize(size);

    for (int i = 0; i < size; i++) {
        vk::DescriptorTableBuilder builder;
        builder.beginDescriptorLayoutSet() // set 0
            .beginDescriptorLayoutSetBinding()
            .defineDescriptorLayoutSetBinding({
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 4096,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            })
            .endDescriptorLayoutSetBinding()
            .endDescriptorLayoutSet()
            .beginDescriptorLayoutSet() // set 1
            .beginDescriptorLayoutSetBinding()
            .defineDescriptorLayoutSetBinding({
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            })
            .endDescriptorLayoutSetBinding()
            .endDescriptorLayoutSet()
            .beginDescriptorLayoutSet() // set 2
            .beginDescriptorLayoutSetBinding()
            .defineDescriptorLayoutSetBinding({
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            })
            .endDescriptorLayoutSetBinding()
            .endDescriptorLayoutSet()
            .beginDescriptorLayoutSet() // set 3
            .beginDescriptorLayoutSetBinding()
            .defineDescriptorLayoutSetBinding({
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 3,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 4,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            })
            .endDescriptorLayoutSetBinding()
            .endDescriptorLayoutSet();

        if (shaderPack_ != nullptr) {
            shaderPack_->defineRuntimeResourceDescriptorSet(builder,
                                                            VK_SHADER_STAGE_VERTEX_BIT |
                                                                VK_SHADER_STAGE_FRAGMENT_BIT,
                                                            VK_SHADER_STAGE_VERTEX_BIT |
                                                                VK_SHADER_STAGE_FRAGMENT_BIT,
                                                            VK_SHADER_STAGE_VERTEX_BIT |
                                                                VK_SHADER_STAGE_FRAGMENT_BIT);
            shaderPack_->defineExecutionDescriptorSet(builder, ShaderPackLoader::Stage::PostRender,
                                                      VK_SHADER_STAGE_VERTEX_BIT |
                                                          VK_SHADER_STAGE_FRAGMENT_BIT);
        }

        descriptorTables_[i] = builder.build(framework->device());

        samplers_[i] = vk::Sampler::create(framework->device(), VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                           VK_SAMPLER_ADDRESS_MODE_REPEAT);
        postPassColorSamplers_[i] =
            vk::Sampler::create(framework->device(), VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        postPassDepthSamplers_[i] =
            vk::Sampler::create(framework->device(), VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    }
}

void PostRenderModule::initImages() {
    auto framework = framework_.lock();
    auto device = framework->device();
    auto vma = framework->vma();
    uint32_t size = framework->recordingContextCount();

    worldPostDepthImages_.resize(size);

    for (int i = 0; i < size; i++) {
        descriptorTables_[i]->bindImage(firstHitDepthImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 2);
        descriptorTables_[i]->bindSamplerImage(postPassColorSamplers_[i], postRenderedImages_[i],
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 3, 0, 0);
        descriptorTables_[i]->bindSamplerImage(postPassDepthSamplers_[i], firstHitDepthImages_[i],
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 3, 1, 0);
        descriptorTables_[i]->bindSamplerImage(postPassColorSamplers_[i], hdrImages_[i],
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 3, 2, 0);
        descriptorTables_[i]->bindSamplerImage(postPassColorSamplers_[i], motionVectorImages_[i],
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 3, 3, 0);
        descriptorTables_[i]->bindSamplerImage(postPassColorSamplers_[i], normalRoughnessImages_[i],
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 3, 4, 0);

        worldPostDepthImages_[i] = vk::DeviceLocalImage::create(
            device, vma, false, width_, height_, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

        if (shaderPack_ != nullptr && shaderPack_->hasRuntimeResources() && shaderPack_->runtimeResourcesReady()) {
            shaderPack_->bindRuntimeResources(descriptorTables_[i], 4, i);
        }
    }
}

void PostRenderModule::initBuffers() {}

void PostRenderModule::initRenderPass() {
    auto device = framework_.lock()->device();
    worldPostColorToDepthRenderPass_ = vk::RenderPassBuilder{}
                                           .beginAttachmentDescription()
                                           .defineAttachmentDescription({
                                               // depth
                                               .format = worldPostDepthImages_[0]->vkFormat(),
                                               .samples = VK_SAMPLE_COUNT_1_BIT,
                                               .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                               .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                               .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                               .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                               .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                           })
                                           .endAttachmentDescription()
                                           .beginAttachmentReference()
                                           .defineAttachmentReference({
                                               .attachment = 0,
                                               .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                           })
                                           .endAttachmentReference()
                                           .beginSubpassDescription()
                                           .defineSubpassDescription({
                                               .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                               .colorAttachmentIndices = {},
                                               .depthStencilAttachmentIndex = 0,
                                           })
                                           .endSubpassDescription()
                                           .build(device);
}

void PostRenderModule::initFrameBuffers() {
    auto framework = framework_.lock();
    auto device = framework->device();
    uint32_t size = framework->recordingContextCount();

    worldPostColorToDepthFramebuffers_.resize(size);

    for (int i = 0; i < size; i++) {
        worldPostColorToDepthFramebuffers_[i] = vk::FramebufferBuilder{}
                                                    .beginAttachment()
                                                    .defineAttachment(worldPostDepthImages_[i])
                                                    .endAttachment()
                                                    .build(device, worldPostColorToDepthRenderPass_);
    }
}

void PostRenderModule::initPipeline() {
    auto framework = framework_.lock();
    auto device = framework->device();
    std::filesystem::path shaderPath = Renderer::folderPath / "shaders";
    fullScreenVertexShader_ = vk::Shader::create(device, (shaderPath / "full_screen_vert.spv").string());

    worldPostColorToDepthVertShader_ =
        vk::Shader::create(device, (shaderPath / "world/post_render/color_to_depth_vert.spv").string());
    worldPostColorToDepthFragShader_ =
        vk::Shader::create(device, (shaderPath / "world/post_render/color_to_depth_frag.spv").string());

    worldPostColorToDepthPipeline_ =
        vk::GraphicsPipelineBuilder{}
            .defineRenderPass(worldPostColorToDepthRenderPass_, 0)
            .beginShaderStage()
            .defineShaderStage(worldPostColorToDepthVertShader_, VK_SHADER_STAGE_VERTEX_BIT)
            .defineShaderStage(worldPostColorToDepthFragShader_, VK_SHADER_STAGE_FRAGMENT_BIT)
            .endShaderStage()
            .defineVertexInputState<void>()
            .defineViewportScissorState({
                .viewport =
                    {
                        .x = 0,
                        .y = 0,
                        .width = static_cast<float>(worldPostDepthImages_[0]->width()),
                        .height = static_cast<float>(worldPostDepthImages_[0]->height()),
                        .minDepth = 0.0,
                        .maxDepth = 1.0,
                    },
                .scissor =
                    {
                        .offset = {.x = 0, .y = 0},
                        .extent =
                            {
                                .width = worldPostDepthImages_[0]->width(),
                                .height = worldPostDepthImages_[0]->height(),
                            },
                    },
            })
            .defineDepthStencilState({
                .depthTestEnable = VK_TRUE,
                .depthWriteEnable = VK_TRUE,
                .depthCompareOp = VK_COMPARE_OP_LESS,
                .depthBoundsTestEnable = VK_FALSE,
                .stencilTestEnable = VK_FALSE,
                .minDepthBounds = 0.0,
                .maxDepthBounds = 1.0,
            })
            .beginColorBlendAttachmentState()
            .endColorBlendAttachmentState()
            .definePipelineLayout(descriptorTables_[0])
            .build(device);
}

std::vector<ExpressionEvaluator::Variable> PostRenderModule::executionExpressionVariables() const {
    return {
        {.name = "RENDER_WIDTH", .value = static_cast<double>(width_)},
        {.name = "RENDER_HEIGHT", .value = static_cast<double>(height_)},
    };
}

double PostRenderModule::evaluateNumericExpression(const std::string &expression,
                                                   const PostRenderModule::ExecutionVariables &variables) {
    return shaderPack_->evaluateNumericExpression(ShaderPackLoader::Stage::PostRender, expression, variables,
                                                  executionExpressionVariables(), true);
}

std::optional<std::reference_wrapper<ShaderPackLoader::VariableConfig>>
PostRenderModule::findExecutionVariableConfig(std::string_view name) {
    auto executionIter = executionVariableConfigs_.find(std::string(name));
    if (executionIter != executionVariableConfigs_.end()) { return std::ref(executionIter->second); }
    return std::nullopt;
}

void PostRenderModule::uploadExecutionBuffer(
    const std::shared_ptr<vk::DeviceLocalBuffer> &executionBuffer,
    PostRenderModuleContext &context,
    const std::unordered_map<std::string, ExecutionVariable> &variables) {
    auto frameworkContext = context.frameworkContext.lock();
    auto framework = frameworkContext->framework.lock();
    shaderPack_->uploadExecutionBuffer(ShaderPackLoader::Stage::PostRender, executionBuffer, variables,
                                       frameworkContext->worldCommandBuffer, context.descriptorTable,
                                       shaderPack_->executionSet(4u), framework->physicalDevice()->mainQueueIndex(),
                                       VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
}

std::shared_ptr<vk::DeviceLocalImage> PostRenderModule::findBuiltInImage(const std::string &name, uint32_t frameIndex) {
    if (name == TARGET_LDR) { return postRenderedImages_[frameIndex]; }
    if (name == TARGET_FIRST_HIT_DEPTH) { return firstHitDepthImages_[frameIndex]; }
    if (name == TARGET_HDR) { return hdrImages_[frameIndex]; }
    if (name == TARGET_MOTION_VECTOR) { return motionVectorImages_[frameIndex]; }
    if (name == TARGET_NORMAL_ROUGHNESS) { return normalRoughnessImages_[frameIndex]; }
    return nullptr;
}

std::shared_ptr<vk::DeviceLocalImage> PostRenderModule::findTargetImage(const std::string &target, uint32_t frameIndex) {
    if (target == TARGET_LDR) { return postRenderedImages_[frameIndex]; }
    if (target == TARGET_FIRST_HIT_DEPTH) { return firstHitDepthImages_[frameIndex]; }

    if (shaderPack_ != nullptr) {
        if (auto runtimeTexture = shaderPack_->findRuntimeTexture(target); runtimeTexture.has_value()) {
            if (runtimeTexture->get().config.imported) { return nullptr; }
            return shaderPack_->findRuntimeVKTexture(runtimeTexture->get(), frameIndex);
        }
    }
    return nullptr;
}

void PostRenderModule::ensureDynamicPipelines() {
    if (isDynamicPipelinesReady_) { return; }
    if (shaderPack_ == nullptr || !shaderPack_->runtimeResourcesReady()) { return; }

#ifdef DEBUG
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;
    auto tPhase = clock::now();
    auto printPhase = [&tPhase](const char *label) {
        auto now = clock::now();
        mcvr::log::debug("PostRenderModule") << "Dynamic pipelines " << label << ": "
                  << ms(now - tPhase).count() << " ms" << std::endl;
        tPhase = now;
    };
#endif

    auto framework = framework_.lock();
    auto device = framework->device();
    uint32_t frameCount = framework->recordingContextCount();
    const auto &shaderPack = shaderPack_->shaderPack();
    const uint32_t executionSet = shaderPack_->executionSet(4u);
    const size_t executionBufferSize = shaderPack.postRenderExecution.variables.size() * sizeof(float);
    // All passes bind one stable address. Inline updates carry each pass value in
    // queue order; changing the shared descriptor would retarget earlier draws.
    const auto stageExecutionBuffer =
        ShaderPack::createPassExecutionBuffer(device, framework->vma(), executionBufferSize);

    fullScreenPasses_.clear();
    passNameToPass_.clear();
    renderPasses_.clear();
    renderPassNameToPass_.clear();
    for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        if (shaderPack_->hasRuntimeResources()) {
            shaderPack_->bindRuntimeResources(descriptorTables_[frameIndex], 4, frameIndex);
        }
    }

    struct ShaderAssignment {
        enum class Type {
            FullScreenFragment,
            RenderVertex,
            RenderFragment,
        };
        Type type;
        size_t index;
        std::string variantName;
    };

    std::vector<ShaderPack::ShaderCreateInfo> shaderRequests;
    std::vector<ShaderAssignment> shaderAssignments;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> fullScreenTargetImages;

    auto hasOutputImage = [](const ShaderPackLoader::ResourceList &resources, std::string_view name) {
        return std::find(resources.images.begin(), resources.images.end(), std::string(name)) !=
               resources.images.end();
    };

    auto colorBlendAttachment = [](bool enableBlend) {
        VkPipelineColorBlendAttachmentState state{
            .blendEnable = enableBlend ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT,
        };
        if (!enableBlend) {
            state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        }
        return state;
    };

    auto firstHitDepthBlendAttachment = []() {
        return VkPipelineColorBlendAttachmentState{
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT,
        };
    };

    for (const auto &passConfig : shaderPack.passes) {
        if (passConfig.stage != ShaderPackLoader::Stage::PostRender) { continue; }
#ifdef DEBUG
        auto tPass = clock::now();
#endif

        switch (passConfig.type) {
            case ShaderPackLoader::PassConfig::Type::FullScreen: {
                if (passConfig.fullScreen.outputs.images.size() != 1 ||
                    passConfig.fullScreen.outputs.images[0] != passConfig.fullScreen.target) {
                    throw std::runtime_error("post_render full_screen pass must only write to its target: " +
                                             passConfig.fullScreen.name);
                }

                auto targetImage = findTargetImage(passConfig.fullScreen.target, 0);
                if (targetImage == nullptr) {
                    throw std::runtime_error("invalid post_render target: " + passConfig.fullScreen.target);
                }
                uint32_t targetViewIndex = 0;
                if (targetImage->layer() > 1) {
                    if (!passConfig.fullScreen.layer.has_value()) {
                        throw std::runtime_error("post_render full_screen array target requires layer: " +
                                                 passConfig.fullScreen.name);
                    }
                    if (*passConfig.fullScreen.layer >= targetImage->layer()) {
                        throw std::runtime_error("post_render full_screen layer out of range: " +
                                                 passConfig.fullScreen.name);
                    }
                    targetViewIndex = *passConfig.fullScreen.layer + 1;
                }

                auto pass = std::make_shared<FullScreenPass>();
                pass->config = passConfig.fullScreen;
                if (executionBufferSize > 0) {
                    pass->executionBuffer = stageExecutionBuffer;
                }
                pass->renderPass = vk::RenderPassBuilder{}
                                       .beginAttachmentDescription()
                                       .defineAttachmentDescription({
                                           .format = targetImage->vkFormat(),
                                           .samples = VK_SAMPLE_COUNT_1_BIT,
                                           .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
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
                                       .endAttachmentReference()
                                       .beginSubpassDescription()
                                       .defineSubpassDescription({
                                           .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                           .colorAttachmentIndices = {0},
                                       })
                                       .endSubpassDescription()
                                       .build(device);
                pass->framebuffers.resize(frameCount);
                for (uint32_t frameIndex = 0; frameIndex < frameCount; frameIndex++) {
                    pass->framebuffers[frameIndex].push_back(
                        vk::FramebufferBuilder{}
                            .beginAttachment()
                            .defineAttachment(findTargetImage(pass->config.target, frameIndex),
                                              targetViewIndex)
                            .endAttachment()
                            .build(device, pass->renderPass));
                }
                shaderAssignments.push_back({ShaderAssignment::Type::FullScreenFragment, fullScreenPasses_.size()});
                shaderRequests.push_back({
                    .path = pass->config.fragmentShaderPath,
                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .definitions = pass->config.definitions,
                    .executionStage = ShaderPackLoader::Stage::PostRender,
                    .executionSet = executionSet,
                });
                fullScreenTargetImages.push_back(targetImage);
                fullScreenPasses_.push_back(pass);
                break;
            }
            case ShaderPackLoader::PassConfig::Type::Render: {
                std::vector<std::string> colorOutputs;
                for (const auto &output : passConfig.render.outputs.images) {
                    if (output != TARGET_FIRST_HIT_DEPTH) { colorOutputs.push_back(output); }
                }
                if (colorOutputs.size() != 1) {
                    throw std::runtime_error("post_render render pass must have exactly one color output: " +
                                             passConfig.render.name);
                }

                auto pass = std::make_shared<RenderPass>();
                pass->config = passConfig.render;
                pass->target = parseRenderContent(pass->config.content);
                pass->colorTarget = colorOutputs[0];
                pass->writesFirstHitDepth = hasOutputImage(pass->config.outputs, TARGET_FIRST_HIT_DEPTH);
                const bool depthTest = pass->config.depthTest.value_or(true);
                const bool depthWrite = pass->config.depthWrite.value_or(renderTargetDefaultDepthWrite(pass->target));
                pass->usesDepthAttachment = depthTest || depthWrite;
                auto colorTargetImage = findTargetImage(pass->colorTarget, 0);
                if (colorTargetImage == nullptr) {
                    throw std::runtime_error("invalid post_render render color target: " + pass->colorTarget);
                }
                if (executionBufferSize > 0) {
                    pass->executionBuffer = stageExecutionBuffer;
                }

                vk::RenderPassBuilder renderPassBuilder;
                auto &attachmentBuilder = renderPassBuilder.beginAttachmentDescription();
                attachmentBuilder.defineAttachmentDescription({
                    .format = colorTargetImage->vkFormat(),
                    .samples = VK_SAMPLE_COUNT_1_BIT,
                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                    .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                });
                if (pass->writesFirstHitDepth) {
                    if (colorTargetImage->width() != firstHitDepthImages_[0]->width() ||
                        colorTargetImage->height() != firstHitDepthImages_[0]->height()) {
                        throw std::runtime_error("post_render first-hit-depth output extent does not match color target: " +
                                                 pass->config.name);
                    }
                    attachmentBuilder.defineAttachmentDescription({
                        .format = firstHitDepthImages_[0]->vkFormat(),
                        .samples = VK_SAMPLE_COUNT_1_BIT,
                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    });
                }
                if (pass->usesDepthAttachment) {
                    if (colorTargetImage->width() != worldPostDepthImages_[0]->width() ||
                        colorTargetImage->height() != worldPostDepthImages_[0]->height()) {
                        throw std::runtime_error("post_render depth attachment extent does not match color target: " +
                                                 pass->config.name);
                    }
                    attachmentBuilder.defineAttachmentDescription({
                        .format = worldPostDepthImages_[0]->vkFormat(),
                        .samples = VK_SAMPLE_COUNT_1_BIT,
                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                        .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    });
                }
                attachmentBuilder.endAttachmentDescription();
                auto &referenceBuilder = renderPassBuilder.beginAttachmentReference()
                                             .defineAttachmentReference({
                                                 .attachment = 0,
                                                 .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                             });
                if (pass->writesFirstHitDepth) {
                    referenceBuilder.defineAttachmentReference({
                        .attachment = 1,
                        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    });
                }
                uint32_t depthAttachmentReferenceIndex = static_cast<uint32_t>(-1);
                if (pass->usesDepthAttachment) {
                    const uint32_t depthAttachmentIndex = pass->writesFirstHitDepth ? 2u : 1u;
                    depthAttachmentReferenceIndex = referenceBuilder.attachmentReferences.size();
                    referenceBuilder.defineAttachmentReference({
                            .attachment = depthAttachmentIndex,
                            .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        });
                }
                referenceBuilder.endAttachmentReference();
                pass->renderPass = renderPassBuilder.beginSubpassDescription()
                        .defineSubpassDescription({
                            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                            .colorAttachmentIndices = pass->writesFirstHitDepth ? std::vector<uint32_t>{0, 1}
                                                                                : std::vector<uint32_t>{0},
                            .depthStencilAttachmentIndex = depthAttachmentReferenceIndex,
                        })
                        .endSubpassDescription()
                        .build(device);

                pass->framebuffers.resize(frameCount);
                for (uint32_t frameIndex = 0; frameIndex < frameCount; frameIndex++) {
                    vk::FramebufferBuilder framebufferBuilder;
                    auto &attachments = framebufferBuilder.beginAttachment()
                                            .defineAttachment(findTargetImage(pass->colorTarget, frameIndex));
                    if (pass->writesFirstHitDepth) {
                        attachments.defineAttachment(firstHitDepthImages_[frameIndex]);
                    }
                    if (pass->usesDepthAttachment) {
                        attachments.defineAttachment(worldPostDepthImages_[frameIndex]);
                    }
                    pass->framebuffers[frameIndex] = attachments.endAttachment().build(device, pass->renderPass);
                }

                for (const auto &[variantName, shaderConfig] : pass->config.shaderConfigs) {
                    shaderAssignments.push_back({
                        ShaderAssignment::Type::RenderVertex,
                        renderPasses_.size(),
                        variantName,
                    });
                    shaderRequests.push_back({
                        .path = shaderConfig.vertexShaderPath,
                        .stage = VK_SHADER_STAGE_VERTEX_BIT,
                        .definitions = pass->config.definitions,
                        .executionStage = ShaderPackLoader::Stage::PostRender,
                        .executionSet = executionSet,
                    });
                    shaderAssignments.push_back({
                        ShaderAssignment::Type::RenderFragment,
                        renderPasses_.size(),
                        variantName,
                    });
                    shaderRequests.push_back({
                        .path = shaderConfig.fragmentShaderPath,
                        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                        .definitions = pass->config.definitions,
                        .executionStage = ShaderPackLoader::Stage::PostRender,
                        .executionSet = executionSet,
                    });
                }
                renderPasses_.push_back(pass);
                break;
            }
            case ShaderPackLoader::PassConfig::Type::RayTracing:
            case ShaderPackLoader::PassConfig::Type::Compute:
                throw std::runtime_error("post_render stage only supports full_screen and render passes");
        }

#ifdef DEBUG
        std::string dbgName;
        switch (passConfig.type) {
            case ShaderPackLoader::PassConfig::Type::FullScreen: dbgName = passConfig.fullScreen.name; break;
            case ShaderPackLoader::PassConfig::Type::Render:     dbgName = passConfig.render.name;     break;
            default: continue;
        }
        mcvr::log::debug("PostRenderModule") << "Create pass '" << dbgName << "': "
                  << ms(clock::now() - tPass).count() << " ms" << std::endl;
#endif
    }

#ifdef DEBUG
    printPhase("create all passes");

    printPhase("collect shader requests");

    ShaderPack::ShaderBatchStats shaderStats;
    auto shaders = shaderPack_->createShaders(device, shaderRequests, &shaderStats);
    std::string compileInfo =
        "build shader modules (" + std::to_string(shaderStats.requestCount) + " requests, " +
        std::to_string(shaderStats.uniqueShaderCount) + " unique, " +
        std::to_string(shaderStats.cacheHitCount) + " cache hits, " +
        std::to_string(shaderStats.cacheMissCount) + " compiled";
    if (shaderStats.cacheReadFailureCount > 0) {
        compileInfo += ", " + std::to_string(shaderStats.cacheReadFailureCount) + " cache read failures";
    }
    compileInfo += ")";
    printPhase(compileInfo.c_str());
#else
    auto shaders = shaderPack_->createShaders(device, shaderRequests);
#endif

    for (size_t shaderIndex = 0; shaderIndex < shaders.size(); shaderIndex++) {
        const auto &assignment = shaderAssignments[shaderIndex];
        switch (assignment.type) {
            case ShaderAssignment::Type::FullScreenFragment:
                fullScreenPasses_[assignment.index]->fragmentShaders.push_back(shaders[shaderIndex]);
                break;
            case ShaderAssignment::Type::RenderVertex:
                renderPasses_[assignment.index]->shaderVariants[assignment.variantName].vertexShader =
                    shaders[shaderIndex];
                break;
            case ShaderAssignment::Type::RenderFragment:
                renderPasses_[assignment.index]->shaderVariants[assignment.variantName].fragmentShader =
                    shaders[shaderIndex];
                break;
        }
    }

#ifdef DEBUG
    std::vector<double> fullScreenBuildTimes(fullScreenPasses_.size(), 0.0);
    mcvr::parallelFor(fullScreenPasses_.size(), [&](size_t passIndex) {
        auto tPass = clock::now();
        auto &pass = fullScreenPasses_[passIndex];
        auto &targetImage = fullScreenTargetImages[passIndex];
        std::vector<std::shared_ptr<vk::GraphicsPipeline>> pipelines;
        pipelines.push_back(
            vk::GraphicsPipelineBuilder{}
                .defineRenderPass(pass->renderPass, 0)
                .beginShaderStage()
                .defineShaderStage(fullScreenVertexShader_, VK_SHADER_STAGE_VERTEX_BIT)
                .defineShaderStage(pass->fragmentShaders[0], VK_SHADER_STAGE_FRAGMENT_BIT)
                .endShaderStage()
                .defineVertexInputState<void>()
                .defineViewportScissorState({
                    .viewport =
                        {
                            .x = 0.0f,
                            .y = 0.0f,
                            .width = static_cast<float>(targetImage->width()),
                            .height = static_cast<float>(targetImage->height()),
                            .minDepth = 0.0f,
                            .maxDepth = 1.0f,
                        },
                    .scissor =
                        {
                            .offset = {.x = 0, .y = 0},
                            .extent = {.width = targetImage->width(), .height = targetImage->height()},
                        },
                })
                .beginColorBlendAttachmentState()
                .defineDefaultColorBlendAttachmentState()
                .endColorBlendAttachmentState()
                .definePipelineLayout(descriptorTables_[0])
                .build(device));
        pass->pipelines = std::move(pipelines);
        fullScreenBuildTimes[passIndex] = ms(clock::now() - tPass).count();
    }, "MCVR_POST_PIPELINE_BUILD_THREADS");
#else
    mcvr::parallelFor(fullScreenPasses_.size(), [&](size_t passIndex) {
        auto &pass = fullScreenPasses_[passIndex];
        auto &targetImage = fullScreenTargetImages[passIndex];
        std::vector<std::shared_ptr<vk::GraphicsPipeline>> pipelines;
        pipelines.push_back(
            vk::GraphicsPipelineBuilder{}
                .defineRenderPass(pass->renderPass, 0)
                .beginShaderStage()
                .defineShaderStage(fullScreenVertexShader_, VK_SHADER_STAGE_VERTEX_BIT)
                .defineShaderStage(pass->fragmentShaders[0], VK_SHADER_STAGE_FRAGMENT_BIT)
                .endShaderStage()
                .defineVertexInputState<void>()
                .defineViewportScissorState({
                    .viewport =
                        {
                            .x = 0.0f,
                            .y = 0.0f,
                            .width = static_cast<float>(targetImage->width()),
                            .height = static_cast<float>(targetImage->height()),
                            .minDepth = 0.0f,
                            .maxDepth = 1.0f,
                        },
                    .scissor =
                        {
                            .offset = {.x = 0, .y = 0},
                            .extent = {.width = targetImage->width(), .height = targetImage->height()},
                        },
                })
                .beginColorBlendAttachmentState()
                .defineDefaultColorBlendAttachmentState()
                .endColorBlendAttachmentState()
                .definePipelineLayout(descriptorTables_[0])
                .build(device));
        pass->pipelines = std::move(pipelines);
    }, "MCVR_POST_PIPELINE_BUILD_THREADS");
#endif

#ifdef DEBUG
    std::vector<double> renderBuildTimes(renderPasses_.size(), 0.0);
    mcvr::parallelFor(renderPasses_.size(), [&](size_t passIndex) {
        auto tPass = clock::now();
        auto &pass = renderPasses_[passIndex];
        const bool depthTest = pass->config.depthTest.value_or(true);
        const bool depthWrite = pass->config.depthWrite.value_or(renderTargetDefaultDepthWrite(pass->target));
        const VkCompareOp depthCompare = parseDepthCompare(pass->config.depthCompare.value_or("less"));

        for (auto &[variantName, variant] : pass->shaderVariants) {
            vk::GraphicsPipelineBuilder pipelineBuilder;
            auto &colorBlendBuilder =
                pipelineBuilder.defineRenderPass(pass->renderPass, 0)
                    .beginShaderStage()
                    .defineShaderStage(variant.vertexShader, VK_SHADER_STAGE_VERTEX_BIT)
                    .defineShaderStage(variant.fragmentShader, VK_SHADER_STAGE_FRAGMENT_BIT)
                    .endShaderStage()
                    .defineVertexInputState<vk::VertexFormat::PBRVertex>()
                    .defineViewportScissorState({
                        .viewport =
                            {
                                .x = 0.0f,
                                .y = 0.0f,
                                .width = static_cast<float>(findTargetImage(pass->colorTarget, 0)->width()),
                                .height = static_cast<float>(findTargetImage(pass->colorTarget, 0)->height()),
                                .minDepth = 0.0f,
                                .maxDepth = 1.0f,
                            },
                        .scissor =
                            {
                                .offset = {.x = 0, .y = 0},
                                .extent = {.width = findTargetImage(pass->colorTarget, 0)->width(),
                                           .height = findTargetImage(pass->colorTarget, 0)->height()},
                            },
                    })
                    .defineDepthStencilState({
                        .depthTestEnable = depthTest ? VK_TRUE : VK_FALSE,
                        .depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE,
                        .depthCompareOp = depthCompare,
                        .depthBoundsTestEnable = VK_FALSE,
                        .stencilTestEnable = VK_FALSE,
                        .minDepthBounds = 0.0f,
                        .maxDepthBounds = 1.0f,
                    })
                    .defineRasterizationState(VkPipelineRasterizationStateCreateInfo{
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                        .depthClampEnable = VK_FALSE,
                        .rasterizerDiscardEnable = VK_FALSE,
                        .polygonMode = VK_POLYGON_MODE_FILL,
                        .cullMode = VK_CULL_MODE_NONE,
                        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                        .depthBiasEnable = VK_FALSE,
                        .depthBiasConstantFactor = 0.0f,
                        .depthBiasClamp = 0.0f,
                        .depthBiasSlopeFactor = 0.0f,
                        .lineWidth = 1.0f,
                    })
                    .beginColorBlendAttachmentState()
                    .defineColorBlendAttachmentState(colorBlendAttachment(pass->config.colorBlend));
            if (pass->writesFirstHitDepth) {
                colorBlendBuilder.defineColorBlendAttachmentState(firstHitDepthBlendAttachment());
            }
            variant.pipeline = colorBlendBuilder.endColorBlendAttachmentState()
                                   .definePipelineLayout(descriptorTables_[0])
                                   .build(device);
            if (variantName == "default") { pass->pipeline = variant.pipeline; }
        }
        renderBuildTimes[passIndex] = ms(clock::now() - tPass).count();
    }, "MCVR_POST_PIPELINE_BUILD_THREADS");
#else
    mcvr::parallelFor(renderPasses_.size(), [&](size_t passIndex) {
        auto &pass = renderPasses_[passIndex];
        const bool depthTest = pass->config.depthTest.value_or(true);
        const bool depthWrite = pass->config.depthWrite.value_or(renderTargetDefaultDepthWrite(pass->target));
        const VkCompareOp depthCompare = parseDepthCompare(pass->config.depthCompare.value_or("less"));
        for (auto &[variantName, variant] : pass->shaderVariants) {
            vk::GraphicsPipelineBuilder pipelineBuilder;
            auto &colorBlendBuilder =
                pipelineBuilder.defineRenderPass(pass->renderPass, 0)
                    .beginShaderStage()
                    .defineShaderStage(variant.vertexShader, VK_SHADER_STAGE_VERTEX_BIT)
                    .defineShaderStage(variant.fragmentShader, VK_SHADER_STAGE_FRAGMENT_BIT)
                    .endShaderStage()
                    .defineVertexInputState<vk::VertexFormat::PBRVertex>()
                    .defineViewportScissorState({
                        .viewport =
                            {
                                .x = 0.0f,
                                .y = 0.0f,
                                .width = static_cast<float>(findTargetImage(pass->colorTarget, 0)->width()),
                                .height = static_cast<float>(findTargetImage(pass->colorTarget, 0)->height()),
                                .minDepth = 0.0f,
                                .maxDepth = 1.0f,
                            },
                        .scissor =
                            {
                                .offset = {.x = 0, .y = 0},
                                .extent = {.width = findTargetImage(pass->colorTarget, 0)->width(),
                                           .height = findTargetImage(pass->colorTarget, 0)->height()},
                            },
                    })
                    .defineDepthStencilState({
                        .depthTestEnable = depthTest ? VK_TRUE : VK_FALSE,
                        .depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE,
                        .depthCompareOp = depthCompare,
                        .depthBoundsTestEnable = VK_FALSE,
                        .stencilTestEnable = VK_FALSE,
                        .minDepthBounds = 0.0f,
                        .maxDepthBounds = 1.0f,
                    })
                    .defineRasterizationState(VkPipelineRasterizationStateCreateInfo{
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                        .depthClampEnable = VK_FALSE,
                        .rasterizerDiscardEnable = VK_FALSE,
                        .polygonMode = VK_POLYGON_MODE_FILL,
                        .cullMode = VK_CULL_MODE_NONE,
                        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                        .depthBiasEnable = VK_FALSE,
                        .depthBiasConstantFactor = 0.0f,
                        .depthBiasClamp = 0.0f,
                        .depthBiasSlopeFactor = 0.0f,
                        .lineWidth = 1.0f,
                    })
                    .beginColorBlendAttachmentState()
                    .defineColorBlendAttachmentState(colorBlendAttachment(pass->config.colorBlend));
            if (pass->writesFirstHitDepth) {
                colorBlendBuilder.defineColorBlendAttachmentState(firstHitDepthBlendAttachment());
            }
            variant.pipeline = colorBlendBuilder.endColorBlendAttachmentState()
                                   .definePipelineLayout(descriptorTables_[0])
                                   .build(device);
            if (variantName == "default") { pass->pipeline = variant.pipeline; }
        }
    }, "MCVR_POST_PIPELINE_BUILD_THREADS");
#endif

#ifdef DEBUG
    for (size_t passIndex = 0; passIndex < fullScreenPasses_.size(); passIndex++) {
        auto &pass = fullScreenPasses_[passIndex];
        mcvr::log::debug("PostRenderModule") << "Build full-screen pass '" << pass->config.name << "': "
                  << fullScreenBuildTimes[passIndex] << " ms" << std::endl;
        if (!passNameToPass_.emplace(pass->config.name, pass).second) {
            throw std::runtime_error("duplicate post_render pass name: " + pass->config.name);
        }
    }

    for (size_t passIndex = 0; passIndex < renderPasses_.size(); passIndex++) {
        auto &pass = renderPasses_[passIndex];
        mcvr::log::debug("PostRenderModule") << "Build render pass '" << pass->config.name << "': "
                  << renderBuildTimes[passIndex] << " ms" << std::endl;
        if (!renderPassNameToPass_.emplace(pass->config.name, pass).second ||
            passNameToPass_.find(pass->config.name) != passNameToPass_.end()) {
            throw std::runtime_error("duplicate post_render pass name: " + pass->config.name);
        }
    }

    printPhase("build all passes");
#else
    for (size_t passIndex = 0; passIndex < fullScreenPasses_.size(); passIndex++) {
        auto &pass = fullScreenPasses_[passIndex];
        if (!passNameToPass_.emplace(pass->config.name, pass).second) {
            throw std::runtime_error("duplicate post_render pass name: " + pass->config.name);
        }
    }

    for (size_t passIndex = 0; passIndex < renderPasses_.size(); passIndex++) {
        auto &pass = renderPasses_[passIndex];
        if (!renderPassNameToPass_.emplace(pass->config.name, pass).second ||
            passNameToPass_.find(pass->config.name) != passNameToPass_.end()) {
            throw std::runtime_error("duplicate post_render pass name: " + pass->config.name);
        }
    }
#endif

    isDynamicPipelinesReady_ = true;
}

PostRenderModuleContext::PostRenderModuleContext(std::shared_ptr<FrameworkContext> frameworkContext,
                                                 std::shared_ptr<WorldPipelineContext> worldPipelineContext,
                                                 std::shared_ptr<PostRenderModule> postRenderModule)
    : WorldModuleContext(frameworkContext, worldPipelineContext),
      postRenderModule(postRenderModule),
      ldrImage(postRenderModule->ldrImages_[frameworkContext->frameIndex]),
      firstHitDepthImage(postRenderModule->firstHitDepthImages_[frameworkContext->frameIndex]),
      hdrImage(postRenderModule->hdrImages_[frameworkContext->frameIndex]),
      motionVectorImage(postRenderModule->motionVectorImages_[frameworkContext->frameIndex]),
      normalRoughnessImage(postRenderModule->normalRoughnessImages_[frameworkContext->frameIndex]),
      worldPostDepthImage(postRenderModule->worldPostDepthImages_[frameworkContext->frameIndex]),
      descriptorTable(postRenderModule->descriptorTables_[frameworkContext->frameIndex]),
      worldPostColorToDepthFramebuffer(
          postRenderModule->worldPostColorToDepthFramebuffers_[frameworkContext->frameIndex]),
      postRenderedImage(postRenderModule->postRenderedImages_[frameworkContext->frameIndex]) {}

void PostRenderModuleContext::render() {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto worldCommandBuffer = context->worldCommandBuffer;
    auto mainQueueIndex = framework->physicalDevice()->mainQueueIndex();

    auto module = postRenderModule.lock();

    auto buffers = Renderer::instance().buffers();
    auto chooseSrc = [](VkImageLayout oldLayout,
                        VkPipelineStageFlags2 fallbackStage,
                        VkAccessFlags2 fallbackAccess,
                        VkPipelineStageFlags2 &outStage,
                        VkAccessFlags2 &outAccess) {
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            outStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            outAccess = 0;
        } else {
            outStage = fallbackStage;
            outAccess = fallbackAccess;
        }
        if (SceneRecordingScope::active() && oldLayout != VK_IMAGE_LAYOUT_UNDEFINED)
            outStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    };

    if (module && module->postRenderedInitialized_.size() > context->frameIndex &&
        module->postRenderedInitialized_[context->frameIndex] == 0) {
        VkImageLayout targetLayout =
#ifdef USE_AMD
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
#else
            (SceneRecordingScope::active() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
#endif
        VkPipelineStageFlags2 dstStage =
#ifdef USE_AMD
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
#else
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
#endif
        VkAccessFlags2 dstAccess =
#ifdef USE_AMD
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
#else
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
#endif
        worldCommandBuffer->barriersBufferImage(
            {}, {{.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                  .srcAccessMask = 0,
                  .dstStageMask = dstStage,
                  .dstAccessMask = dstAccess,
                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                  .newLayout = targetLayout,
                  .srcQueueFamilyIndex = mainQueueIndex,
                  .dstQueueFamilyIndex = mainQueueIndex,
                  .image = postRenderedImage,
                  .subresourceRange = vk::wholeColorSubresourceRange}});
        postRenderedImage->imageLayout() = targetLayout;
        module->postRenderedInitialized_[context->frameIndex] = 1;
    }

    auto ensureLayout = [&](const std::shared_ptr<vk::DeviceLocalImage> &img,
                            VkImageLayout targetLayout,
                            VkPipelineStageFlags2 dstStage,
                            VkAccessFlags2 dstAccess) {
        if (!img || img->imageLayout() == targetLayout) return;
        VkPipelineStageFlags2 srcStage = 0;
        VkAccessFlags2 srcAccess = 0;
        chooseSrc(img->imageLayout(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, srcStage, srcAccess);
        worldCommandBuffer->barriersBufferImage(
            {}, {{.srcStageMask = srcStage,
                  .srcAccessMask = srcAccess,
                  .dstStageMask = dstStage,
                  .dstAccessMask = dstAccess,
                  .oldLayout = img->imageLayout(),
                  .newLayout = targetLayout,
                  .srcQueueFamilyIndex = mainQueueIndex,
                  .dstQueueFamilyIndex = mainQueueIndex,
                  .image = img,
                  .subresourceRange = vk::wholeColorSubresourceRange}});
        img->imageLayout() = targetLayout;
    };

    ensureLayout(postRenderedImage,
#ifdef USE_AMD
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
#else
                 (SceneRecordingScope::active() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
#endif
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    ensureLayout(firstHitDepthImage, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    if (worldPostDepthImage && worldPostDepthImage->imageLayout() != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        VkPipelineStageFlags2 srcStage = 0;
        VkAccessFlags2 srcAccess = 0;
        chooseSrc(worldPostDepthImage->imageLayout(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, srcStage, srcAccess);
        worldCommandBuffer->barriersBufferImage(
            {}, {{.srcStageMask = srcStage,
                  .srcAccessMask = srcAccess,
                  .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                  .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  .oldLayout = worldPostDepthImage->imageLayout(),
                  .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  .srcQueueFamilyIndex = mainQueueIndex,
                  .dstQueueFamilyIndex = mainQueueIndex,
                  .image = worldPostDepthImage,
                  .subresourceRange = vk::wholeDepthSubresourceRange}});
        worldPostDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    descriptorTable->bindBuffer(buffers->worldUniformBuffer(), 1, 0);
    descriptorTable->bindBuffer(buffers->skyUniformBuffer(), 1, 1);
    descriptorTable->bindBuffer(Renderer::instance().buffers()->textureMappingBuffer(), 2, 0);

    {
        VkPipelineStageFlags2 srcStageDepth = 0;
        VkAccessFlags2 srcAccessDepth = 0;
        chooseSrc(worldPostDepthImage->imageLayout(),
                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                  srcStageDepth, srcAccessDepth);

        VkPipelineStageFlags2 srcStageFirstHit = 0;
        VkAccessFlags2 srcAccessFirstHit = 0;
        chooseSrc(firstHitDepthImage->imageLayout(),
                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                  srcStageFirstHit, srcAccessFirstHit);

        worldCommandBuffer->barriersBufferImage(
            {}, {{
                     .srcStageMask = srcStageDepth,
                     .srcAccessMask = srcAccessDepth,
                     .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     .oldLayout = worldPostDepthImage->imageLayout(),
                     .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     .srcQueueFamilyIndex = mainQueueIndex,
                     .dstQueueFamilyIndex = mainQueueIndex,
                     .image = worldPostDepthImage,
                     .subresourceRange = vk::wholeDepthSubresourceRange,
                 },
                 {
                     .srcStageMask = srcStageFirstHit,
                     .srcAccessMask = srcAccessFirstHit,
                     .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     .oldLayout = firstHitDepthImage->imageLayout(),
                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                     .srcQueueFamilyIndex = mainQueueIndex,
                     .dstQueueFamilyIndex = mainQueueIndex,
                     .image = firstHitDepthImage,
                     .subresourceRange = vk::wholeColorSubresourceRange,
                 }});
    }
    worldPostDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    firstHitDepthImage->imageLayout() = VK_IMAGE_LAYOUT_GENERAL;

    worldCommandBuffer->beginRenderPass({
        .renderPass = module->worldPostColorToDepthRenderPass_,
        .framebuffer = worldPostColorToDepthFramebuffer,
        .renderAreaExtent = {worldPostDepthImage->width(), worldPostDepthImage->height()},
        .clearValues = {{.depthStencil = {.depth = 1.0f}}},
    });
    worldPostDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    worldCommandBuffer->bindDescriptorTable(descriptorTable, VK_PIPELINE_BIND_POINT_GRAPHICS)
        ->bindGraphicsPipeline(module->worldPostColorToDepthPipeline_)
        ->draw(3, 1)
        ->endRenderPass();
    worldPostDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    {
        VkPipelineStageFlags2 srcStageLdr = 0;
        VkAccessFlags2 srcAccessLdr = 0;
        chooseSrc(ldrImage->imageLayout(),
                  mcvr::postColorSourceStages(VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT),
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                  srcStageLdr, srcAccessLdr);

        VkPipelineStageFlags2 srcStagePost = 0;
        VkAccessFlags2 srcAccessPost = 0;
        chooseSrc(postRenderedImage->imageLayout(),
                  mcvr::postColorSourceStages(VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT),
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                  srcStagePost, srcAccessPost);

        worldCommandBuffer->barriersBufferImage(
            {}, {{
                     .srcStageMask = srcStageLdr,
                     .srcAccessMask = srcAccessLdr,
                     .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                     VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     .oldLayout = ldrImage->imageLayout(),
                     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     .srcQueueFamilyIndex = mainQueueIndex,
                     .dstQueueFamilyIndex = mainQueueIndex,
                     .image = ldrImage,
                     .subresourceRange = vk::wholeColorSubresourceRange,
                 },
                 {
                     .srcStageMask = srcStagePost,
                     .srcAccessMask = srcAccessPost,
                     .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     .oldLayout = postRenderedImage->imageLayout(),
                     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     .srcQueueFamilyIndex = mainQueueIndex,
                     .dstQueueFamilyIndex = mainQueueIndex,
                     .image = postRenderedImage,
                     .subresourceRange = vk::wholeColorSubresourceRange,
                 }});
    }
    ldrImage->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    postRenderedImage->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    // TODO: add to command buffer
    VkImageBlit imageBlit{};
    imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlit.srcSubresource.mipLevel = 0;
    imageBlit.srcSubresource.baseArrayLayer = 0;
    imageBlit.srcSubresource.layerCount = 1;
    imageBlit.srcOffsets[0] = {0, 0, 0};
    imageBlit.srcOffsets[1] = {static_cast<int>(ldrImage->width()), static_cast<int>(ldrImage->height()), 1};
    imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlit.dstSubresource.mipLevel = 0;
    imageBlit.dstSubresource.baseArrayLayer = 0;
    imageBlit.dstSubresource.layerCount = 1;
    imageBlit.dstOffsets[0] = {0, 0, 0};
    imageBlit.dstOffsets[1] = {static_cast<int>(postRenderedImage->width()),
                               static_cast<int>(postRenderedImage->height()), 1};

    vkCmdBlitImage(worldCommandBuffer->vkCommandBuffer(), ldrImage->vkImage(), ldrImage->imageLayout(),
                   postRenderedImage->vkImage(), postRenderedImage->imageLayout(), 1, &imageBlit, VK_FILTER_LINEAR);

    module->ensureDynamicPipelines();
    if (module->shaderPack_ != nullptr && module->shaderPack_->hasRuntimeResources()) {
        module->shaderPack_->bindRuntimeResources(descriptorTable, 4, context->frameIndex);
    }
    if (module->isDynamicPipelinesReady_ && !module->shaderPack_->execution(ShaderPackLoader::Stage::PostRender).commands.empty()) {
        PostRenderModule::ExecutionVariables variables;
        variables.reserve(module->globalVariables_.size());
        for (const auto &[name, value] : module->globalVariables_) {
            auto config = module->findExecutionVariableConfig(name);
            variables.emplace(name,
                              PostRenderModule::ExecutionVariable{
                                  .name = name,
                                  .value = value,
                                  .type = config.has_value() ? config->get().type : "",
                              });
        }

        auto executePass = [&](const std::string &passName, ShaderPack::ExecutionVariables &passVariables) {
            auto frameworkContext = this->frameworkContext.lock();
            auto framework = frameworkContext->framework.lock();
            auto worldCommandBuffer = frameworkContext->worldCommandBuffer;
            const uint32_t frameIndex = frameworkContext->frameIndex;
            const uint32_t queueIndex = framework->physicalDevice()->mainQueueIndex();

            auto addColorImageBarrier = [&](const std::shared_ptr<vk::DeviceLocalImage> &image,
                                            VkImageLayout newLayout) {
                if (image == nullptr) { return; }
                VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                VkAccessFlags2 dstAccess = VK_ACCESS_2_SHADER_READ_BIT;
                if (newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                    dstStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                    dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                } else if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
                           newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                    dstStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                    dstAccess = newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_2_TRANSFER_READ_BIT
                                                                                  : VK_ACCESS_2_TRANSFER_WRITE_BIT;
                }
                worldCommandBuffer->barriersBufferImage(
                    {},
                    {{
                        .srcStageMask = image->imageLayout() == VK_IMAGE_LAYOUT_UNDEFINED ?
                                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT :
                                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                        .srcAccessMask = image->imageLayout() == VK_IMAGE_LAYOUT_UNDEFINED ?
                                             0 :
                                             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                        .dstStageMask = dstStage,
                        .dstAccessMask = dstAccess,
                        .oldLayout = image->imageLayout(),
                        .newLayout = newLayout,
                        .srcQueueFamilyIndex = queueIndex,
                        .dstQueueFamilyIndex = queueIndex,
                        .image = image,
                        .subresourceRange = vk::wholeColorSubresourceRange,
                    }});
                image->imageLayout() = newLayout;
            };

            auto addDepthImageBarrier = [&](const std::shared_ptr<vk::DeviceLocalImage> &image,
                                            VkImageLayout newLayout) {
                if (image == nullptr || image->imageLayout() == newLayout) { return; }
                worldCommandBuffer->barriersBufferImage(
                    {},
                    {{
                        .srcStageMask = image->imageLayout() == VK_IMAGE_LAYOUT_UNDEFINED ?
                                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT :
                                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        .srcAccessMask = image->imageLayout() == VK_IMAGE_LAYOUT_UNDEFINED ?
                                             0 :
                                             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        .oldLayout = image->imageLayout(),
                        .newLayout = newLayout,
                        .srcQueueFamilyIndex = queueIndex,
                        .dstQueueFamilyIndex = queueIndex,
                        .image = image,
                        .subresourceRange = vk::wholeDepthSubresourceRange,
                    }});
                image->imageLayout() = newLayout;
            };

            auto prepareInputs = [&](const ShaderPackLoader::ResourceList &inputs) {
                for (const std::string &input : inputs.images) {
                    if (auto builtInImage = module->findBuiltInImage(input, frameIndex); builtInImage != nullptr) {
                        addColorImageBarrier(builtInImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        continue;
                    }
                    auto runtimeTexture = module->shaderPack_->findRuntimeTexture(input);
                    if (!runtimeTexture.has_value()) { throw std::runtime_error("unknown post_render input: " + input); }
                    VkImageLayout layout =
                        runtimeTexture->get().config.imported || !runtimeTexture->get().config.storageBinding.has_value() ?
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
                            VK_IMAGE_LAYOUT_GENERAL;
                    addColorImageBarrier(module->shaderPack_->findRuntimeVKTexture(runtimeTexture->get(), frameIndex),
                                         layout);
                }
            };

            if (auto passIter = module->passNameToPass_.find(passName); passIter != module->passNameToPass_.end()) {
                auto pass = passIter->second;
                auto targetImage = module->findTargetImage(pass->config.target, frameIndex);
                if (targetImage == nullptr) {
                    throw std::runtime_error("unknown post_render target: " + pass->config.target);
                }

                prepareInputs(pass->config.inputs);

                if (pass->config.target == PostRenderModule::TARGET_LDR ||
                    pass->config.target == PostRenderModule::TARGET_FIRST_HIT_DEPTH) {
                    addColorImageBarrier(targetImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                } else {
                    auto runtimeTexture = module->shaderPack_->findRuntimeTexture(pass->config.target);
                    if (!runtimeTexture.has_value() || runtimeTexture->get().config.imported) {
                        throw std::runtime_error("invalid post_render output target: " + pass->config.target);
                    }
                    addColorImageBarrier(module->shaderPack_->findRuntimeVKTexture(runtimeTexture->get(), frameIndex),
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                }

                if (pass->executionBuffer != nullptr) {
                    module->uploadExecutionBuffer(pass->executionBuffer, *this, passVariables);
                }

                worldCommandBuffer->beginRenderPass({
                    .renderPass = pass->renderPass,
                    .framebuffer = pass->framebuffers[frameIndex][0],
                    .renderAreaExtent = {targetImage->width(), targetImage->height()},
                    .clearValues = {},
                });
                targetImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                worldCommandBuffer->bindGraphicsPipeline(pass->pipelines[0])
                    ->bindDescriptorTable(descriptorTable, VK_PIPELINE_BIND_POINT_GRAPHICS)
                    ->draw(3, 1)
                    ->endRenderPass();
                targetImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                return;
            }

            auto renderPassIter = module->renderPassNameToPass_.find(passName);
            if (renderPassIter == module->renderPassNameToPass_.end()) {
                throw std::runtime_error("unknown post_render pass: " + passName);
            }

            auto pass = renderPassIter->second;
            prepareInputs(pass->config.inputs);
            auto colorTargetImage = module->findTargetImage(pass->colorTarget, frameIndex);
            if (colorTargetImage == nullptr) {
                throw std::runtime_error("invalid post_render render color target: " + pass->colorTarget);
            }
            addColorImageBarrier(colorTargetImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            if (pass->writesFirstHitDepth) {
                addColorImageBarrier(firstHitDepthImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            }
            if (pass->usesDepthAttachment) {
                addDepthImageBarrier(worldPostDepthImage, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            }

            if (pass->executionBuffer != nullptr) {
                module->uploadExecutionBuffer(pass->executionBuffer, *this, passVariables);
            }

            bool hasWork = false;
            auto entityPostRenderDataBatch = Renderer::instance().world()->entities()->entityPostBatch();
            const int postRenderFlag = PostRenderModule::renderTargetPostFlag(pass->target);
            if (entityPostRenderDataBatch != nullptr) {
                for (const auto &entity : entityPostRenderDataBatch->entities) {
                    if (entity->postRenderFlag != postRenderFlag) { continue; }

                    for (uint32_t geometryIndex = 0; geometryIndex < entity->geometryCount; geometryIndex++) {
                        const std::string &contentName =
                            geometryIndex < entity->geometryContentNames.size() ?
                                entity->geometryContentNames[geometryIndex] :
                                std::string{};
                        if (findShaderVariant(*pass, contentName) != nullptr) {
                            hasWork = true;
                            break;
                        }
                    }
                    if (hasWork) { break; }
                }
            }
            if (!hasWork) { return; }

            worldCommandBuffer->beginRenderPass({
                .renderPass = pass->renderPass,
                .framebuffer = pass->framebuffers[frameIndex],
                .renderAreaExtent = {colorTargetImage->width(), colorTargetImage->height()},
                .clearValues = {},
            });
            colorTargetImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            if (pass->writesFirstHitDepth) {
                firstHitDepthImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            if (pass->usesDepthAttachment) {
                worldPostDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }

            worldCommandBuffer->bindDescriptorTable(descriptorTable, VK_PIPELINE_BIND_POINT_GRAPHICS);
            {
                auto entityPostRenderDataBatch = Renderer::instance().world()->entities()->entityPostBatch();
                const int postRenderFlag = PostRenderModule::renderTargetPostFlag(pass->target);
                const RenderPass::ShaderVariant *boundVariant = nullptr;
                for (const auto &entity : entityPostRenderDataBatch->entities) {
                    if (entity->postRenderFlag != postRenderFlag) { continue; }

                    for (uint32_t j = 0; j < entity->geometryCount; j++) {
                        const std::string &contentName =
                            j < entity->geometryContentNames.size() ? entity->geometryContentNames[j] : std::string{};
                        const RenderPass::ShaderVariant *variant = findShaderVariant(*pass, contentName);
                        if (variant == nullptr || variant->pipeline == nullptr) { continue; }
                        const std::string variantName = findShaderVariantName(*pass, contentName);
                        // if (!variantName.empty()) {
                        //     logShaderVariantSelectionOnce(pass->config.name, contentName, variantName);
                        // }
                        if (variant != boundVariant) {
                            worldCommandBuffer->bindGraphicsPipeline(variant->pipeline);
                            boundVariant = variant;
                        }

                        auto &vertexBuffer = entity->vertexBuffers[j];
                        auto &indexBuffer = entity->indexBuffers[j];

                        worldCommandBuffer->bindVertexBuffers(vertexBuffer)
                            ->bindIndexBuffer(indexBuffer)
                            ->drawIndexed(entity->indexCounts[j], 1);
                    }
                }
            }
            worldCommandBuffer->endRenderPass();
            postRenderedImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            if (pass->writesFirstHitDepth) {
                firstHitDepthImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            if (pass->usesDepthAttachment) {
                worldPostDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }
        };
        module->shaderPack_->executeCommands(
            ShaderPackLoader::Stage::PostRender,
            module->shaderPack_->execution(ShaderPackLoader::Stage::PostRender).commands, variables,
            module->executionExpressionVariables(), true, 1u << 16, executePass);
        for (auto &[name, value] : module->globalVariables_) {
            auto iter = variables.find(name);
            if (iter != variables.end()) { value = iter->second.value; }
        }
    }

    auto finalizePostRenderTarget = [&]() {
        VkPipelineStageFlags2 srcStagePost = 0;
        VkAccessFlags2 srcAccessPost = 0;
        chooseSrc(postRenderedImage->imageLayout(),
                  mcvr::postColorSourceStages(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT),
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                  srcStagePost, srcAccessPost);

        worldCommandBuffer->barriersBufferImage(
            {}, {{
                     .srcStageMask = srcStagePost,
                     .srcAccessMask = srcAccessPost,
                     .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     .oldLayout = postRenderedImage->imageLayout(),
#ifdef USE_AMD
                     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
#else
                     .newLayout = (SceneRecordingScope::active() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
#endif
                     .srcQueueFamilyIndex = mainQueueIndex,
                     .dstQueueFamilyIndex = mainQueueIndex,
                     .image = postRenderedImage,
                     .subresourceRange = vk::wholeColorSubresourceRange,
                 }});
#ifdef USE_AMD
        postRenderedImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
#else
        postRenderedImage->imageLayout() = (SceneRecordingScope::active() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
#endif
    };
    finalizePostRenderTarget();
    // Stable producer/consumer boundary for MainTarget depth conversion. Overlay
    // recording precedes world recording, while queue submission has world first.
    ensureLayout(firstHitDepthImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
