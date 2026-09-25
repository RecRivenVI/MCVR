#include "core/render/ui_coverage.hpp"
#include "core/logging.hpp"
#include "core/vulkan/image_format.hpp"
#include "core/render/modules/ui_module.hpp"

#include "core/diagnostics/draw_state_trace.hpp"
#include "core/render/buffers.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/world.hpp"

#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <regex>
#include <stdexcept>

UIModule::UIModule() {}

UIModule::~UIModule() {
#ifdef DEBUG
    mcvr::log::info("UiModule") << "UI deconstruct" << std::endl;
#endif
}

void UIModule::init(std::shared_ptr<Framework> framework) {
    framework_ = framework;

    initOverlayDescriptorTablesAndFrameSamplers();
    initOverlayDrawImages();
    initMainAliasImages();
    initDiagramDrawImages();
    initOverlayDrawRenderPass();
    initMainDepthPass();
    initDiagramDrawRenderPass();
    initOverlayDrawFrameBuffers();
    initDiagramDrawFrameBuffers();

    initOverlayPostImages();
    initSpiderBlurImages();
    initOverlayPostRenderPass();
    initOverlayPostFrameBuffers();
    initSpiderBlurFrameBuffers();
    initOverlayPostPipelineTypes();
    initOverlayPostPipelines();

    uint32_t size = framework->swapchain()->imageCount();
    contexts_.resize(size);
    for (int i = 0; i < size; i++) {
        contexts_[i] = UIModuleContext::create(framework->contexts()[i], shared_from_this());
    }

#ifdef DEBUG
    mcvr::log::info("UiModule") << "UI init" << std::endl;
#endif
}

void UIModule::resize(std::shared_ptr<Framework> framework) {
    if (framework == nullptr) { throw std::invalid_argument("Cannot resize UI without a framework"); }
    if (framework->swapchain()->imageCount() == 0) {
        throw std::runtime_error("Cannot resize UI for a swapchain with no images");
    }

    std::shared_ptr<UIModuleContext> persistentState = lastActiveContext_.lock();
    if (persistentState == nullptr && !contexts_.empty()) { persistentState = contexts_.front(); }

    framework_ = framework;

    // Contexts own aliases to all per-frame resources. Release them first. The caller has
    // already completed old GPU work and rebuilt the Framework contexts at this point.
    contexts_.clear();
    lastActiveContext_.reset();

    // Post pipelines bake the old viewport and scissor. Dynamic draw pipelines keep both
    // states dynamic and retain their registration, shader objects, and compatible render pass.
    overlayPostPipelines_.clear();
    framebufferPipelines_.clear();
    framebufferClearPipelines_.clear();
    framebufferBlitPipelines_.clear();
    framebufferRenderPasses_.clear();
    mainDepthPipeline_.reset();
    mainDepthRenderPass_.reset();

    // A framebuffer must be destroyed before any image view it references.
    overlayDrawFramebuffers_.clear();
    diagramDrawFramebuffers_.clear();
    overlayPostFramebuffers_.clear();
    spiderLargeBlurFramebuffers_.clear();
    spiderSmallBlurFramebuffers_.clear();
    mainDepthFramebuffers_.clear();

    overlayDescriptorTables_.clear();
    mainDepthDescriptorTables_.clear();
    mainDepthSourceSamplers_.clear();
    overlayDrawColorImageSamplers_.clear();
    diagramDrawColorSamplers_.clear();
    diagramDrawDepthSamplers_.clear();
    overlayPostColorImageSamplers_.clear();
    spiderSmallBlurSamplers_.clear();

    overlayDrawColorImages_.clear();
    overlayDrawDepthStencilImages_.clear();
    overlayDrawDepthStencilViewIndices_.clear();
    mainColorAliasImages_.clear();
    mainDepthAliasImages_.clear();
    diagramDrawColorImages_.clear();
    diagramDrawDepthImages_.clear();
    overlayPostColorImages_.clear();
    spiderSmallBlurImages_.clear();

    initOverlayDescriptorTablesAndFrameSamplers();
    initOverlayDrawImages();
    initMainAliasImages();
    initDiagramDrawImages();
    initOverlayPostImages();
    initSpiderBlurImages();
    initMainDepthPass();
    initOverlayDrawFrameBuffers();
    initDiagramDrawFrameBuffers();
    initOverlayPostFrameBuffers();
    initSpiderBlurFrameBuffers();
    initOverlayPostPipelines();

    const uint32_t size = framework->swapchain()->imageCount();
    contexts_.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
        contexts_[i] = UIModuleContext::create(framework->contexts()[i], shared_from_this());
        if (persistentState != nullptr) { contexts_[i]->copyPersistentStateFrom(*persistentState); }
    }
    lastActiveContext_ = contexts_.front();
}

std::vector<std::shared_ptr<UIModuleContext>> &UIModule::contexts() {
    return contexts_;
}

std::vector<std::shared_ptr<vk::DescriptorTable>> &UIModule::overlayDescriptorTables() {
    return overlayDescriptorTables_;
}

const std::vector<OverlayDynamicDrawShaderInfo> &UIModule::overlayDynamicDrawShaders() const {
    return overlayDynamicDrawShaders_;
}

uint32_t UIModule::registerOverlayDrawShader(const std::string &key,
                                             uint32_t vertexFormatType,
                                             uint32_t drawMode,
                                             uint32_t uniformSize,
                                             const std::string &vertexShaderPath,
                                             const std::string &fragmentShaderPath,
                                             const std::string &tessellationControlShaderPath,
                                             const std::string &tessellationEvaluationShaderPath,
                                             uint32_t patchControlPoints,
                                             const std::optional<vk::VertexLayoutInfo> &customVertexLayout,
                                             const std::unordered_map<std::string, std::string> &definitions) {
    enum class OverlayAttributeNumericKind {
        FLOAT,
        SINT,
        UINT,
    };

    struct OverlayAttributeType {
        OverlayAttributeNumericKind numericKind;
    };

    auto overlayTopologyForDrawMode = [](uint32_t overlayDrawMode) -> VkPrimitiveTopology {
        switch (overlayDrawMode) {
            // Minecraft's LINES mode duplicates both endpoints and indexes them as two
            // triangles so the line shader can produce a stable screen-space width.
            case 0: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
            case 7: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            default: throw std::runtime_error("Unsupported overlay draw mode");
        }
    };
    auto overlayVertexLayoutFor = [](uint32_t overlayVertexFormatType) -> vk::VertexLayoutInfo {
        switch (overlayVertexFormatType) {
            case 0: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionColorTexLightNormal>();
            case 1: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionColorTexOverlayLightNormal>();
            case 2: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionTexColorLight>();
            case 3: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionOnly>();
            case 4: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionColor>();
            case 5: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionColorNormal>();
            case 6: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionColorLight>();
            case 7: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionTex>();
            case 8: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionTexColor>();
            case 9: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionColorTexLight>();
            case 10: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionTexLightColor>();
            case 11: return vk::Vertex::vertexLayoutInfo<vk::VertexFormat::PositionTexColorNormal>();
            default: throw std::runtime_error("Unsupported overlay vertex format type");
        }
    };
    auto parseOverlayVertexInputs =
        [](const std::string &overlayVertexShaderPath) -> std::unordered_map<uint32_t, OverlayAttributeType> {
        std::ifstream sourceFile(overlayVertexShaderPath, std::ios::binary);
        if (!sourceFile.is_open()) { return {}; }

        std::string sourceText{std::istreambuf_iterator<char>(sourceFile), std::istreambuf_iterator<char>()};
        std::regex inputPattern(
            R"(layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s+(float|int|uint|vec[234]|ivec[234]|uvec[234])\s+\w+\s*;)");
        std::unordered_map<uint32_t, OverlayAttributeType> result;

        for (std::sregex_iterator it(sourceText.begin(), sourceText.end(), inputPattern), end; it != end; ++it) {
            uint32_t location = static_cast<uint32_t>(std::stoul((*it)[1].str()));
            std::string type = (*it)[2].str();

            OverlayAttributeNumericKind numericKind = OverlayAttributeNumericKind::FLOAT;
            if (!type.empty() && type[0] == 'i') {
                numericKind = OverlayAttributeNumericKind::SINT;
            } else if (!type.empty() && type[0] == 'u') {
                numericKind = OverlayAttributeNumericKind::UINT;
            }

            result[location] = OverlayAttributeType{numericKind};
        }

        return result;
    };
    auto adaptOverlayAttributeFormat =
        [](VkFormat format, OverlayAttributeNumericKind numericKind) -> VkFormat {
        if (numericKind == OverlayAttributeNumericKind::FLOAT) {
            switch (format) {
                case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SSCALED;
                case VK_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_USCALED;
                case VK_FORMAT_R16_SINT: return VK_FORMAT_R16_SSCALED;
                case VK_FORMAT_R16_UINT: return VK_FORMAT_R16_USCALED;
                case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SSCALED;
                case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_USCALED;
                default: return format;
            }
        }
        return format;
    };
    auto makeOverlayVertexLayout = [&](uint32_t overlayVertexFormatType,
                                       const std::string &overlayVertexShaderPath) -> vk::VertexLayoutInfo {
        vk::VertexLayoutInfo layout = overlayVertexLayoutFor(overlayVertexFormatType);
        std::unordered_map<uint32_t, OverlayAttributeType> inputs = parseOverlayVertexInputs(overlayVertexShaderPath);

        for (VkVertexInputAttributeDescription &attribute : layout.attributeDescriptions) {
            auto it = inputs.find(attribute.location);
            if (it == inputs.end()) { continue; }
            attribute.format = adaptOverlayAttributeFormat(attribute.format, it->second.numericKind);
        }

        return layout;
    };

    auto existing = overlayDynamicDrawShaderIds_.find(key);
    if (existing != overlayDynamicDrawShaderIds_.end()) { return existing->second; }

    auto framework = framework_.lock();
    if (framework == nullptr) { throw std::runtime_error("Framework is not available"); }
    OverlayDynamicDrawShaderInfo info{};
    info.key = key;
    info.vertexFormatType = vertexFormatType;
    info.drawMode = drawMode;
    info.uniformSize = uniformSize;
    info.vertexShaderPath = vertexShaderPath;
    info.tessellationControlShaderPath = tessellationControlShaderPath;
    info.tessellationEvaluationShaderPath = tessellationEvaluationShaderPath;
    info.fragmentShaderPath = fragmentShaderPath;
    info.patchControlPoints = patchControlPoints;
    info.definitions = definitions;
    info.topology = overlayTopologyForDrawMode(drawMode);
    info.shaders.vertexShader = vk::Shader::create(
        framework->device(), vertexShaderPath, VK_SHADER_STAGE_VERTEX_BIT, definitions);
    info.shaders.fragmentShader = vk::Shader::create(
        framework->device(), fragmentShaderPath, VK_SHADER_STAGE_FRAGMENT_BIT, definitions);
    if (tessellationControlShaderPath.empty() != tessellationEvaluationShaderPath.empty())
        throw std::runtime_error("Tessellation requires both control and evaluation shaders");
    if (!tessellationControlShaderPath.empty()) {
        if (patchControlPoints != 4 || drawMode != 7)
            throw std::runtime_error("Levitite tessellation requires four-control-point QUADS");
        info.shaders.tessellationControlShader = vk::Shader::create(
            framework->device(), tessellationControlShaderPath,
            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, definitions);
        info.shaders.tessellationEvaluationShader = vk::Shader::create(
            framework->device(), tessellationEvaluationShaderPath,
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, definitions);
    } else if (patchControlPoints != 0) {
        throw std::runtime_error("Patch control points require tessellation shaders");
    }

    vk::DynamicGraphicsPipelineBuilder builder{1};
    vk::VertexLayoutInfo vertexLayout = customVertexLayout.has_value()
        ? *customVertexLayout : makeOverlayVertexLayout(vertexFormatType, vertexShaderPath);
    info.vertexLayout = vertexLayout;
    info.customVertexLayout = customVertexLayout.has_value();
    builder.defineRenderPass(overlayDrawRenderPass_, 0);
    auto &stages = builder.beginShaderStage();
    stages.defineShaderStage(info.shaders.vertexShader, VK_SHADER_STAGE_VERTEX_BIT);
    if (info.shaders.tessellationControlShader) {
        stages.defineShaderStage(info.shaders.tessellationControlShader,
            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
        stages.defineShaderStage(info.shaders.tessellationEvaluationShader,
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
    }
    stages.defineShaderStage(info.shaders.fragmentShader, VK_SHADER_STAGE_FRAGMENT_BIT)
        .endShaderStage();
    builder.defineVertexInputState(vertexLayout);
    if (patchControlPoints != 0) builder.definePatchControlPoints(patchControlPoints);
    else builder.defineInputAssemblyState(info.topology);
    info.pipeline = builder.definePipelineLayout(overlayDescriptorTables_[0]).build(framework->device());

    // Optional warmup must not alter live descriptor ranges if compilation fails.
    bool descriptorRangeChanged = Renderer::instance().buffers()->registerOverlayDrawUniformSize(uniformSize);
    if (descriptorRangeChanged) {
        std::lock_guard lock(overlayDescriptorMutex_);
        std::fill(overlayDescriptorTableDirty_.begin(), overlayDescriptorTableDirty_.end(), true);
    }
    uint32_t shaderId = overlayDynamicDrawShaders_.size();
    overlayDynamicDrawShaderIds_[key] = shaderId;
    overlayDynamicDrawShaders_.push_back(std::move(info));
    return shaderId;
}

const OverlayDynamicDrawShaderInfo &UIModule::overlayDrawShaderInfo(uint32_t shaderId) const {
    return overlayDynamicDrawShaders_.at(shaderId);
}

std::string UIModule::resourceDiagnostics() const {
    return "dynamicDrawShaders=" + std::to_string(overlayDynamicDrawShaders_.size()) +
           " dynamicDrawIds=" + std::to_string(overlayDynamicDrawShaderIds_.size()) +
           " fbPipelines=" + std::to_string(framebufferPipelines_.size()) +
           " fbPasses=" + std::to_string(framebufferRenderPasses_.size()) +
           " fbClearPipelines=" + std::to_string(framebufferClearPipelines_.size()) +
           " fbBlitPipelines=" + std::to_string(framebufferBlitPipelines_.size()) +
           " descriptorTables=" + std::to_string(overlayDescriptorTables_.size()) +
           " drawColorImages=" + std::to_string(overlayDrawColorImages_.size()) +
           " drawDepthImages=" + std::to_string(overlayDrawDepthStencilImages_.size()) +
           " postColorImages=" + std::to_string(overlayPostColorImages_.size()) +
           " postPipelines=" + std::to_string(overlayPostPipelines_.size()) +
           " mainDepthPipelines=" + std::to_string(mainDepthPipeline_ != nullptr ? 1 : 0) +
           " contexts=" + std::to_string(contexts_.size()) +
           " textureBindings=" + std::to_string(overlayTextureBindings_.size()) +
           " uiPtViews=" + std::to_string(ponderScenes.size()) +
           " uiPtPipelineCreates=" + std::to_string(uiPtPipelineCreates) +
           " uiPtViewCreates=" + std::to_string(uiPtViewCreates) +
           " uiPtGeometryBuilds=" + std::to_string(uiPtGeometryBuilds) +
           " uiPtMaterialUpdates=" + std::to_string(uiPtMaterialUpdates) +
           " uiPtCompositeCreates=" + std::to_string(uiPtCompositeCreates);
}

std::shared_ptr<vk::DescriptorTable> UIModule::createOverlayDescriptorTable() {
    auto framework = framework_.lock();

    return vk::DescriptorTableBuilder{}
        .definePushConstant({VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t)})
        .beginDescriptorLayoutSet()
        .beginDescriptorLayoutSetBinding()
        .defineDescriptorLayoutSetBinding({
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 4096,
            .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        })
        .defineDescriptorLayoutSetBinding({
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        })
        .defineDescriptorLayoutSetBinding({
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        })
        .defineDescriptorLayoutSetBinding({
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        })
        .defineDescriptorLayoutSetBinding({
            .binding = 4,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        })
        .defineDescriptorLayoutSetBinding({
            .binding = 5,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        })
        .endDescriptorLayoutSetBinding()
        .endDescriptorLayoutSet()
        .beginDescriptorLayoutSet()
        .beginDescriptorLayoutSetBinding()
        .defineDescriptorLayoutSetBinding({
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        })
        .defineDescriptorLayoutSetBinding({
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        })
        .endDescriptorLayoutSetBinding()
        .endDescriptorLayoutSet()
        .build(framework->device());
}

void UIModule::bindOverlayDescriptorTableResources(std::shared_ptr<vk::DescriptorTable> descriptorTable,
                                                   uint32_t frameIndex) {
    const uint64_t descriptorTableObject = reinterpret_cast<uint64_t>(descriptorTable.get());
    const uint64_t pipelineLayout = mcvr::diagnostics::handleValue(descriptorTable->vkPipelineLayout());
    const auto descriptorSetHandle = [&](uint32_t set) -> uint64_t {
        const auto &sets = descriptorTable->descriptorSet();
        return set < sets.size() ? mcvr::diagnostics::handleValue(sets[set]) : 0;
    };
    const auto bindImage = [&](const std::shared_ptr<vk::Sampler> &sampler,
                               const std::shared_ptr<vk::DeviceLocalImage> &image,
                               VkImageLayout layout,
                               int32_t set,
                               int32_t binding,
                               int32_t index,
                               uint32_t viewIndex = 0) {
        descriptorTable->bindSamplerImage(sampler, image, layout, static_cast<uint32_t>(set),
                                          static_cast<uint32_t>(binding), static_cast<uint32_t>(index), viewIndex);
        mcvr::diagnostics::recordDescriptorImage(
            frameIndex, descriptorTableObject, pipelineLayout,
            descriptorSetHandle(static_cast<uint32_t>(set)), set, binding, index,
            reinterpret_cast<uint64_t>(sampler.get()), mcvr::diagnostics::handleValue(sampler->vkSamper()),
            reinterpret_cast<uint64_t>(image.get()), mcvr::diagnostics::handleValue(image->vkImage()),
            mcvr::diagnostics::handleValue(image->vkImageView(viewIndex)), static_cast<uint32_t>(layout));
    };

    for (auto &[index, binding] : overlayTextureBindings_) {
        auto image = binding.image;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (binding.frameAlias == OverlayTextureBinding::FrameAlias::MainColor) {
            image = mainColorAliasImages_.at(frameIndex);
        } else if (binding.frameAlias == OverlayTextureBinding::FrameAlias::MainDepth) {
            image = mainDepthAliasImages_.at(frameIndex);
            layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }
        if (image != nullptr && (mcvr::framebuffer::formatAspects(image->vkFormat()) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
            layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }
        if (binding.sampler != nullptr && image != nullptr) {
            bindImage(binding.sampler, image, layout, 0, 0, index);
        }
    }

    bindImage(overlayDrawColorImageSamplers_[frameIndex], overlayDrawColorImages_[frameIndex],
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0);
    bindImage(diagramDrawColorSamplers_[frameIndex], diagramDrawColorImages_[frameIndex],
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 2, 0);
    bindImage(diagramDrawDepthSamplers_[frameIndex], diagramDrawDepthImages_[frameIndex],
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 0, 3, 0);
    bindImage(spiderSmallBlurSamplers_[frameIndex], spiderSmallBlurImages_[frameIndex],
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 4, 0);
    bindImage(overlayPostColorImageSamplers_[frameIndex], overlayPostColorImages_[frameIndex],
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 5, 0);

    auto drawUniformBuffer = Renderer::instance().buffers()->overlayDrawUniformBuffer();
    const uint32_t drawUniformRange = Renderer::instance().buffers()->overlayDrawUniformDescriptorRange();
    descriptorTable->bindBufferRange(drawUniformBuffer, 1, 0, 0, drawUniformRange);
    mcvr::diagnostics::recordDescriptorBuffer(
        frameIndex, descriptorTableObject, pipelineLayout, descriptorSetHandle(1), 1, 0,
        reinterpret_cast<uint64_t>(drawUniformBuffer.get()),
        mcvr::diagnostics::handleValue(drawUniformBuffer->vkBuffer()), 0, drawUniformRange,
        drawUniformBuffer->size());

    auto postUniformBuffer = Renderer::instance().buffers()->overlayPostUniformBuffer();
    const uint32_t postUniformRange = Renderer::instance().buffers()->overlayPostUniformDescriptorRange();
    descriptorTable->bindBufferRange(postUniformBuffer, 1, 1, 0, postUniformRange);
    mcvr::diagnostics::recordDescriptorBuffer(
        frameIndex, descriptorTableObject, pipelineLayout, descriptorSetHandle(1), 1, 1,
        reinterpret_cast<uint64_t>(postUniformBuffer.get()),
        mcvr::diagnostics::handleValue(postUniformBuffer->vkBuffer()), 0, postUniformRange,
        postUniformBuffer->size());
}

void UIModule::bindTexture(std::shared_ptr<vk::Sampler> sampler,
                           std::shared_ptr<vk::DeviceLocalImage> image,
                           int index) {
    std::lock_guard lock(overlayDescriptorMutex_);
    const auto existing = overlayTextureBindings_.find(index);
    if (existing != overlayTextureBindings_.end() &&
        existing->second.frameAlias == OverlayTextureBinding::FrameAlias::None &&
        existing->second.sampler == sampler && existing->second.image == image) {
        return;
    }
    overlayTextureBindings_[index] = {
        .sampler = sampler,
        .image = image,
    };

    std::fill(overlayDescriptorTableDirty_.begin(), overlayDescriptorTableDirty_.end(), true);
}

void UIModule::bindFrameAlias(int index, bool depth, std::shared_ptr<vk::Sampler> sampler) {
    std::lock_guard lock(overlayDescriptorMutex_);
    overlayTextureBindings_[index] = {
        .sampler = std::move(sampler),
        .image = nullptr,
        .frameAlias = depth ? OverlayTextureBinding::FrameAlias::MainDepth
                            : OverlayTextureBinding::FrameAlias::MainColor,
    };
    std::fill(overlayDescriptorTableDirty_.begin(), overlayDescriptorTableDirty_.end(), true);
}

void UIModule::unbindTexture(int index) {
    std::lock_guard lock(overlayDescriptorMutex_);
    overlayTextureBindings_.erase(index);
    std::fill(overlayDescriptorTableDirty_.begin(), overlayDescriptorTableDirty_.end(), true);
}

void UIModule::prepareOverlayDescriptorTable(uint32_t frameIndex) {
    std::lock_guard lock(overlayDescriptorMutex_);
    if (frameIndex >= overlayDescriptorTableDirty_.size()) {
        throw std::out_of_range("Overlay descriptor frame index is out of range");
    }
    if (overlayDescriptorTableDirty_[frameIndex]) refreshOverlayDescriptorTable(frameIndex);
}

bool UIModule::defaultStencilAvailable() const {
    return defaultStencilAvailable_;
}

void UIModule::refreshOverlayDescriptorTable(uint32_t frameIndex) {
    std::lock_guard lock(overlayDescriptorMutex_);
    auto framework = framework_.lock();
    auto &frr = framework->frameResourceRetainer();

    auto oldDescriptorTable = overlayDescriptorTables_[frameIndex];
    auto descriptorTable = createOverlayDescriptorTable();
    bindOverlayDescriptorTableResources(descriptorTable, frameIndex);

    overlayDescriptorTables_[frameIndex] = descriptorTable;
    if (frameIndex < contexts_.size() && contexts_[frameIndex] != nullptr) {
        contexts_[frameIndex]->overlayDescriptorTable = descriptorTable;
    }
    overlayDescriptorTableDirty_[frameIndex] = false;

    frr.retain(oldDescriptorTable);
}

void UIModule::initOverlayDescriptorTablesAndFrameSamplers() {
    auto framework = framework_.lock();

    uint32_t size = framework->swapchain()->imageCount();
    overlayDescriptorTables_.resize(size);
    overlayDescriptorTableDirty_.assign(size, true);
    overlayDrawColorImageSamplers_.resize(size);
    diagramDrawColorSamplers_.resize(size);
    diagramDrawDepthSamplers_.resize(size);
    overlayPostColorImageSamplers_.resize(size);
    spiderSmallBlurSamplers_.resize(size);

    for (int i = 0; i < size; i++) {
        overlayDescriptorTables_[i] = createOverlayDescriptorTable();

        overlayDrawColorImageSamplers_[i] = vk::Sampler::create(
            framework->device(), VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        diagramDrawColorSamplers_[i] = vk::Sampler::create(
            framework->device(), VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        diagramDrawDepthSamplers_[i] = vk::Sampler::create(
            framework->device(), VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        overlayPostColorImageSamplers_[i] = vk::Sampler::create(
            framework->device(), VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        spiderSmallBlurSamplers_[i] = vk::Sampler::create(
            framework->device(), VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    }
}

void UIModule::initDiagramDrawImages() {
    auto framework = framework_.lock();

    uint32_t size = framework->swapchain()->imageCount();
    diagramDrawColorImages_.resize(size);
    diagramDrawDepthImages_.resize(size);

    for (uint32_t i = 0; i < size; ++i) {
        diagramDrawColorImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, framework->swapchain()->vkExtent().width,
            framework->swapchain()->vkExtent().height, 1, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        diagramDrawDepthImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, framework->swapchain()->vkExtent().width,
            framework->swapchain()->vkExtent().height, 1, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

        overlayDescriptorTables_[i]->bindSamplerImage(
            diagramDrawColorSamplers_[i], diagramDrawColorImages_[i],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 2, 0);
        overlayDescriptorTables_[i]->bindSamplerImage(
            diagramDrawDepthSamplers_[i], diagramDrawDepthImages_[i],
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 0, 3, 0);
    }
}

void UIModule::initOverlayDrawImages() {
    auto framework = framework_.lock();

    uint32_t size = framework->swapchain()->imageCount();
    overlayDrawColorImages_.resize(size);
    overlayDrawDepthStencilImages_.resize(size);
    overlayDrawDepthStencilViewIndices_.assign(size, 0);

    VkFormatProperties depthStencilProperties{};
    vkGetPhysicalDeviceFormatProperties(framework->physicalDevice()->vkPhysicalDevice(),
                                        VK_FORMAT_D32_SFLOAT_S8_UINT, &depthStencilProperties);
    constexpr VkFormatFeatureFlags requiredDepthStencilFeatures =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    defaultStencilAvailable_ =
        (depthStencilProperties.optimalTilingFeatures & requiredDepthStencilFeatures) == requiredDepthStencilFeatures;
    const VkFormat depthFormat = defaultStencilAvailable_ ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;

    for (int i = 0; i < size; i++) {
        overlayDrawColorImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, framework->swapchain()->vkExtent().width,
            framework->swapchain()->vkExtent().height, 1, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
#ifdef USE_AMD
                | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
#endif
        );
        overlayDrawDepthStencilImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, framework->swapchain()->vkExtent().width,
            framework->swapchain()->vkExtent().height, 1, depthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (defaultStencilAvailable_) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = overlayDrawDepthStencilImages_[i]->vkImage();
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = depthFormat;
            viewInfo.subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };
            overlayDrawDepthStencilImages_[i]->addImageView(viewInfo);
            overlayDrawDepthStencilViewIndices_[i] = 1;
        }

        overlayDescriptorTables_[i]->bindSamplerImage(overlayDrawColorImageSamplers_[i], overlayDrawColorImages_[i],
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0);
    }
}

void UIModule::initMainAliasImages() {
    auto framework = framework_.lock();
    const uint32_t size = framework->swapchain()->imageCount();
    const VkExtent2D extent = framework->swapchain()->vkExtent();
    const VkFormat depthFormat = overlayDrawDepthStencilImages_.front()->vkFormat();
    mainColorAliasImages_.resize(size);
    mainDepthAliasImages_.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
        mainColorAliasImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, extent.width, extent.height, 1,
            VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
        mainDepthAliasImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, extent.width, extent.height, 1, depthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
}

void UIModule::initMainDepthPass() {
    auto framework = framework_.lock();
    constexpr uint32_t pushConstantSize = 32;
    const uint32_t size = framework->swapchain()->imageCount();
    mainDepthDescriptorTables_.resize(size);
    mainDepthSourceSamplers_.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
        mainDepthDescriptorTables_[i] = vk::DescriptorTableBuilder{}
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
            .definePushConstant({VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushConstantSize})
            .build(framework->device());
        mainDepthSourceSamplers_[i] = vk::Sampler::create(
            framework->device(), VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    }

    mainDepthRenderPass_ = vk::RenderPassBuilder{}
        .beginAttachmentDescription()
        .defineAttachmentDescription({
            .format = overlayDrawDepthStencilImages_.front()->vkFormat(),
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        })
        .endAttachmentDescription()
        .beginAttachmentReference()
        .defineAttachmentReference({0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL})
        .endAttachmentReference()
        .beginSubpassDescription()
        .defineSubpassDescription({
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .depthStencilAttachmentIndex = 0,
        })
        .endSubpassDescription()
        .build(framework->device());

    mainDepthFramebuffers_.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
        mainDepthFramebuffers_[i] = vk::FramebufferBuilder{}
            .beginAttachment()
            .defineAttachment(overlayDrawDepthStencilImages_[i], overlayDrawDepthStencilViewIndices_[i])
            .endAttachment()
            .build(framework->device(), mainDepthRenderPass_);
    }

    auto shaderPath = Renderer::folderPath / "shaders/overlay";
    auto vertex = vk::Shader::create(framework->device(), (shaderPath / "clear_vert.spv").string());
    auto fragment = vk::Shader::create(framework->device(), (shaderPath / "main_depth_frag.spv").string());
    vk::DynamicGraphicsPipelineBuilder builder(0);
    builder.defineRenderPass(mainDepthRenderPass_, 0)
        .beginShaderStage()
        .defineShaderStage(vertex, VK_SHADER_STAGE_VERTEX_BIT)
        .defineShaderStage(fragment, VK_SHADER_STAGE_FRAGMENT_BIT)
        .endShaderStage();
    vk::VertexLayoutInfo emptyLayout{};
    builder.defineVertexInputState(emptyLayout);
    mainDepthPipeline_ = builder.defineInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .definePipelineLayout(mainDepthDescriptorTables_.front())
        .build(framework->device());
}

void UIModule::initOverlayDrawRenderPass() {
    auto framework = framework_.lock();

    overlayDrawRenderPass_ = vk::RenderPassBuilder{}
                                 .beginAttachmentDescription()
                                 .defineAttachmentDescription(VkAttachmentDescription{
                                     // color
                                     .format = overlayDrawColorImages_[0]->vkFormat(),
                                     .samples = VK_SAMPLE_COUNT_1_BIT,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                     .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                     .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
#ifdef USE_AMD
                                     .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
#else
                                     .initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                     .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
#endif
                                 })
                                 .defineAttachmentDescription(VkAttachmentDescription{
                                     // depth
                                     .format = overlayDrawDepthStencilImages_[0]->vkFormat(),
                                     .samples = VK_SAMPLE_COUNT_1_BIT,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                     .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                     .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                                     .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                     .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                 })
                                 .endAttachmentDescription()
                                 .beginAttachmentReference()
                                 .defineAttachmentReference({
                                     .attachment = 0,
                                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 })
                                 .defineAttachmentReference({
                                     .attachment = 1,
                                     .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                 })
                                 .endAttachmentReference()
                                 .beginSubpassDescription()
                                 .defineSubpassDescription({
                                     .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     .colorAttachmentIndices = {0},
                                     .depthStencilAttachmentIndex = 1,
                                 })
                                 .endSubpassDescription()
                                 .build(framework->device());
}

void UIModule::initOverlayDrawFrameBuffers() {
    auto framework = framework_.lock();

    uint32_t size = framework->swapchain()->imageCount();
    overlayDrawFramebuffers_.resize(size);

    for (int i = 0; i < size; i++) {
        overlayDrawFramebuffers_[i] = vk::FramebufferBuilder{}
                                          .beginAttachment()
                                          .defineAttachment(overlayDrawColorImages_[i])
                                          .defineAttachment(overlayDrawDepthStencilImages_[i],
                                                            overlayDrawDepthStencilViewIndices_[i])
                                          .endAttachment()
                                          .build(framework->device(), overlayDrawRenderPass_);
    }
}

void UIModule::initDiagramDrawRenderPass() {
    auto framework = framework_.lock();

    diagramDrawRenderPass_ = vk::RenderPassBuilder{}
                                 .beginAttachmentDescription()
                                 .defineAttachmentDescription(VkAttachmentDescription{
                                     .format = diagramDrawColorImages_[0]->vkFormat(),
                                     .samples = VK_SAMPLE_COUNT_1_BIT,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                     .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                     .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                     .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 })
                                 .defineAttachmentDescription(VkAttachmentDescription{
                                     .format = diagramDrawDepthImages_[0]->vkFormat(),
                                     .samples = VK_SAMPLE_COUNT_1_BIT,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
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
                                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 })
                                 .defineAttachmentReference({
                                     .attachment = 1,
                                     .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                 })
                                 .endAttachmentReference()
                                 .beginSubpassDescription()
                                 .defineSubpassDescription({
                                     .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     .colorAttachmentIndices = {0},
                                     .depthStencilAttachmentIndex = 1,
                                 })
                                 .endSubpassDescription()
                                 .build(framework->device());
}

void UIModule::initDiagramDrawFrameBuffers() {
    auto framework = framework_.lock();

    uint32_t size = framework->swapchain()->imageCount();
    diagramDrawFramebuffers_.resize(size);

    for (uint32_t i = 0; i < size; ++i) {
        diagramDrawFramebuffers_[i] = vk::FramebufferBuilder{}
                                          .beginAttachment()
                                          .defineAttachment(diagramDrawColorImages_[i])
                                          .defineAttachment(diagramDrawDepthImages_[i])
                                          .endAttachment()
                                          .build(framework->device(), diagramDrawRenderPass_);
    }
}

void UIModule::initOverlayPostImages() {
    auto framework = framework_.lock();

    uint32_t size = framework->swapchain()->imageCount();
    overlayPostColorImages_.resize(size);

    for (int i = 0; i < size; i++) {
        overlayPostColorImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, framework->swapchain()->vkExtent().width,
            framework->swapchain()->vkExtent().height, 1, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        );

        overlayDescriptorTables_[i]->bindSamplerImage(
            overlayPostColorImageSamplers_[i], overlayPostColorImages_[i],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 5, 0);
    }
}

void UIModule::initSpiderBlurImages() {
    auto framework = framework_.lock();
    uint32_t size = framework->swapchain()->imageCount();
    spiderSmallBlurImages_.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
        spiderSmallBlurImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false,
            framework->swapchain()->vkExtent().width,
            framework->swapchain()->vkExtent().height, 1, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        overlayDescriptorTables_[i]->bindSamplerImage(
            spiderSmallBlurSamplers_[i], spiderSmallBlurImages_[i],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 4, 0);
    }
}

void UIModule::initOverlayPostRenderPass() {
    auto framework = framework_.lock();

    overlayPostRenderPass_ = vk::RenderPassBuilder{}
                                 .beginAttachmentDescription()
                                 .defineAttachmentDescription({
                                     // color
                                     .format = overlayPostColorImages_[0]->vkFormat(),
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
                                 .endAttachmentReference()
                                 .beginSubpassDescription()
                                 .defineSubpassDescription({
                                     .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     .colorAttachmentIndices = {0},
                                 })
                                 .endSubpassDescription()
                                 .build(framework->device());
}

void UIModule::initOverlayPostFrameBuffers() {
    auto framework = framework_.lock();

    uint32_t size = framework->swapchain()->imageCount();
    overlayPostFramebuffers_.resize(size);

    for (int i = 0; i < size; i++) {
        overlayPostFramebuffers_[i] = vk::FramebufferBuilder{}
                                          .beginAttachment()
                                          .defineAttachment(overlayPostColorImages_[i])
                                          .endAttachment()
                                          .build(framework->device(), overlayPostRenderPass_);
    }
}

void UIModule::initSpiderBlurFrameBuffers() {
    auto framework = framework_.lock();
    uint32_t size = framework->swapchain()->imageCount();
    spiderLargeBlurFramebuffers_.resize(size);
    spiderSmallBlurFramebuffers_.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
        spiderLargeBlurFramebuffers_[i] = vk::FramebufferBuilder{}
            .beginAttachment().defineAttachment(diagramDrawColorImages_[i]).endAttachment()
            .build(framework->device(), overlayPostRenderPass_);
        spiderSmallBlurFramebuffers_[i] = vk::FramebufferBuilder{}
            .beginAttachment().defineAttachment(spiderSmallBlurImages_[i]).endAttachment()
            .build(framework->device(), overlayPostRenderPass_);
    }
}

void UIModule::initOverlayPostPipelineTypes() {
    std::filesystem::path shaderPath = Renderer::folderPath / "shaders";
    overlayPostPipelineInfos_[BLUR] = {
        .vertexShaderFile = (shaderPath / "overlay/post/blur_vert.spv").string(),
        .fragmentShaderFile = (shaderPath / "overlay/post/blur_frag.spv").string(),
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    overlayPostPipelineInfos_[DIAGRAM] = {
        .vertexShaderFile = (shaderPath / "overlay/post/diagram_vert.spv").string(),
        .fragmentShaderFile = (shaderPath / "overlay/post/diagram_frag.spv").string(),
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    overlayPostPipelineInfos_[CREEPER] = {
        .vertexShaderFile = (shaderPath / "overlay/post/entity_effect_vert.spv").string(),
        .fragmentShaderFile = (shaderPath / "overlay/post/creeper_frag.spv").string(),
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    overlayPostPipelineInfos_[SPIDER] = {
        .vertexShaderFile = (shaderPath / "overlay/post/entity_effect_vert.spv").string(),
        .fragmentShaderFile = (shaderPath / "overlay/post/spider_frag.spv").string(),
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    overlayPostPipelineInfos_[INVERT] = {
        .vertexShaderFile = (shaderPath / "overlay/post/entity_effect_vert.spv").string(),
        .fragmentShaderFile = (shaderPath / "overlay/post/invert_frag.spv").string(),
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    overlayPostPipelineInfos_[SPIDER_BLUR_MAIN_H15] = {
        .vertexShaderFile = (shaderPath / "overlay/post/entity_effect_vert.spv").string(),
        .fragmentShaderFile = (shaderPath / "overlay/post/spider_blur_main_h15_frag.spv").string(),
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    overlayPostPipelineInfos_[SPIDER_BLUR_TEMP_V15] = {
        .vertexShaderFile = (shaderPath / "overlay/post/entity_effect_vert.spv").string(),
        .fragmentShaderFile = (shaderPath / "overlay/post/spider_blur_temp_v15_frag.spv").string(),
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    overlayPostPipelineInfos_[SPIDER_BLUR_MAIN_H7] = {
        .vertexShaderFile = (shaderPath / "overlay/post/entity_effect_vert.spv").string(),
        .fragmentShaderFile = (shaderPath / "overlay/post/spider_blur_main_h7_frag.spv").string(),
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    overlayPostPipelineInfos_[SPIDER_BLUR_TEMP_V7] = {
        .vertexShaderFile = (shaderPath / "overlay/post/entity_effect_vert.spv").string(),
        .fragmentShaderFile = (shaderPath / "overlay/post/spider_blur_temp_v7_frag.spv").string(),
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
}

void UIModule::initOverlayPostPipelines() {
    auto framework = framework_.lock();

    for (auto overlayPipelineInfo : overlayPostPipelineInfos_) {
        auto [type, info] = overlayPipelineInfo;
        auto [vertexShaderFile, fragmentShaderFile, topology] = info;

        if (!overlayPostPipelineShaders_.contains(type)) {
            overlayPostPipelineShaders_[type] = {
                .vertexShader = vk::Shader::create(framework->device(), vertexShaderFile),
                .fragmentShader = vk::Shader::create(framework->device(), fragmentShaderFile),
            };

#ifdef DEBUG
            {
                std::stringstream ss;
                ss << "UI Post Vertex Shader Type " << type;
                std::string vertName = ss.str();

                VkDebugUtilsObjectNameInfoEXT nameInfo = {};
                nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
                nameInfo.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
                nameInfo.objectHandle = (uint64_t)overlayPostPipelineShaders_[type].vertexShader->vkShaderModule();
                nameInfo.pObjectName = vertName.c_str();

                vkSetDebugUtilsObjectNameEXT(framework->device()->vkDevice(), &nameInfo);
            }

            {
                std::stringstream ss;
                ss << "UI Post Fragment Shader Type " << type;
                std::string fragName = ss.str();

                VkDebugUtilsObjectNameInfoEXT nameInfo = {};
                nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
                nameInfo.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
                nameInfo.objectHandle = (uint64_t)overlayPostPipelineShaders_[type].fragmentShader->vkShaderModule();
                nameInfo.pObjectName = fragName.c_str();

                vkSetDebugUtilsObjectNameEXT(framework->device()->vkDevice(), &nameInfo);
            }
#endif
        }

        overlayPostPipelines_[type] =
            vk::GraphicsPipelineBuilder{}
                .defineRenderPass(overlayPostRenderPass_, 0)
                .beginShaderStage()
                .defineShaderStage(overlayPostPipelineShaders_[type].vertexShader, VK_SHADER_STAGE_VERTEX_BIT)
                .defineShaderStage(overlayPostPipelineShaders_[type].fragmentShader, VK_SHADER_STAGE_FRAGMENT_BIT)
                .endShaderStage()
                .defineVertexInputState<void>()
                .defineViewportScissorState({
                    .viewport =
                        {
                            .x = 0,
                            .y = 0,
                            .width = static_cast<float>(framework->swapchain()->vkExtent().width),
                            .height = static_cast<float>(framework->swapchain()->vkExtent().height),
                            .minDepth = 0.0,
                            .maxDepth = 1.0,
                        },
                    .scissor =
                        {
                            .offset = {.x = 0, .y = 0},
                            .extent = framework->swapchain()->vkExtent(),
                        },
                })
                .defineDepthStencilState({
                    .depthTestEnable = VK_TRUE,
                    .depthWriteEnable = VK_TRUE,
                    .depthCompareOp = VK_COMPARE_OP_LESS,
                    .depthBoundsTestEnable = VK_FALSE,
                    .stencilTestEnable = VK_FALSE,
                })
                .beginColorBlendAttachmentState()
                .defineDefaultColorBlendAttachmentState() // color
                .endColorBlendAttachmentState()
                .definePipelineLayout(overlayDescriptorTables_[0])
                .build(framework->device());
    }
}

UIModuleContext::UIModuleContext(std::shared_ptr<FrameworkContext> context, std::shared_ptr<UIModule> uiModule)
    : frameworkContext(context),
      uiModule(uiModule),
      overlayDescriptorTable(uiModule->overlayDescriptorTables_[context->frameIndex]),
      overlayDrawColorImage(uiModule->overlayDrawColorImages_[context->frameIndex]),
      overlayDrawDepthStencilImage(uiModule->overlayDrawDepthStencilImages_[context->frameIndex]),
      overlayDrawDepthStencilViewIndex(uiModule->overlayDrawDepthStencilViewIndices_[context->frameIndex]),
      mainColorAliasImage(uiModule->mainColorAliasImages_[context->frameIndex]),
      mainDepthAliasImage(uiModule->mainDepthAliasImages_[context->frameIndex]),
      mainDepthFramebuffer(uiModule->mainDepthFramebuffers_[context->frameIndex]),
      mainDepthDescriptorTable(uiModule->mainDepthDescriptorTables_[context->frameIndex]),
      overlayDrawFramebuffer(uiModule->overlayDrawFramebuffers_[context->frameIndex]),
      overlayPostColorImage(uiModule->overlayPostColorImages_[context->frameIndex]),
      spiderSmallBlurImage(uiModule->spiderSmallBlurImages_[context->frameIndex]),
      overlayDrawColorImageSampler(uiModule->overlayDrawColorImageSamplers_[context->frameIndex]),
      overlayPostFramebuffer(uiModule->overlayPostFramebuffers_[context->frameIndex]),
      spiderLargeBlurFramebuffer(uiModule->spiderLargeBlurFramebuffers_[context->frameIndex]),
      spiderSmallBlurFramebuffer(uiModule->spiderSmallBlurFramebuffers_[context->frameIndex]),
      diagramDrawColorImage(uiModule->diagramDrawColorImages_[context->frameIndex]),
      diagramDrawDepthImage(uiModule->diagramDrawDepthImages_[context->frameIndex]),
      diagramDrawFramebuffer(uiModule->diagramDrawFramebuffers_[context->frameIndex]) {
    diagramStateSaved = false;
    overlayScissorEnabled = VK_FALSE;
    overlayScissor = {
        .offset = {0, 0},
        .extent = context->swapchain->vkExtent(),
    };
    overlayScissorGl = {0, 0, static_cast<int>(context->swapchain->vkExtent().width), static_cast<int>(context->swapchain->vkExtent().height)};
    overlayViewportGl = overlayScissorGl;
    overlayViewport = {
        .x = 0,
        .y = 0,
        .width = static_cast<float>(context->swapchain->vkExtent().width),
        .height = static_cast<float>(context->swapchain->vkExtent().height),
        .minDepth = 0.0,
        .maxDepth = 1.0,
    };

    overlayBlendEnabled = VK_FALSE;
    overlayColorBlendEquation = {
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
    };
    overlayColorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    overlayColorLogicOpEnable = VK_FALSE;
    overlayColorLogicOp = VK_LOGIC_OP_COPY;
    overlayBlendConstants = {0.f, 0.f, 0.f, 0.f};

    overlayDepthTestEnable = VK_FALSE;
    overlayDepthWriteEnable = VK_TRUE;
    overlayDepthCompareOp = VK_COMPARE_OP_LESS;
    overlayStencilTestEnable = VK_FALSE;
    overlayFailOp = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP};
    overlayPassOp = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP};
    overlayDepthFailOp = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP};
    overlayCompareOp = {VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_ALWAYS};
    overlayReference = {0, 0};
    overlayCompareMask = {0xffffffff, 0xffffffff};
    overlayWriteMask = {0xffffffff, 0xffffffff};

    overlayCullMode = VK_CULL_MODE_NONE;
    overlayFrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    overlayPolygonMode = VK_POLYGON_MODE_FILL;
    overlayDepthBiasEnable = VK_FALSE;
    overlayDepthBiasConstantFactor = {0.0, 0.0, 0.0};
    overlayDepthBiasClamp = {0.0, 0.0, 0.0};
    overlayDepthBiasSlopeFactor = {0.0, 0.0, 0.0};
    overlayLineWidth = 1.0;

    overlayClearColors = {1.0, 1.0, 1.0, 1.0};
    overlayClearDepth = 1.0;
    overlayClearStencil = 0xffffffff;
}

void UIModuleContext::syncToCommandBuffer() {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    auto commandBuffer = context->overlayCommandBuffer->vkCommandBuffer();

    // ------------ VkPipelineViewportStateCreateInfo ------------
    /* VK_DYNAMIC_STATE_VIEWPORT */
    overlayViewport.x = static_cast<float>(overlayViewportGl[0]);
    overlayViewport.y = static_cast<float>(drawExtent().height) - overlayViewportGl[1] - overlayViewportGl[3];
    overlayViewport.width = static_cast<float>(overlayViewportGl[2]);
    overlayViewport.height = static_cast<float>(overlayViewportGl[3]);
    vkCmdSetViewport(commandBuffer, 0, 1, &overlayViewport);

    /* VK_DYNAMIC_STATE_SCISSOR */
    if (overlayScissorEnabled)
        vkCmdSetScissor(commandBuffer, 0, 1, &overlayScissor);
    else {
        VkRect2D full_scissor = {
            .offset = {0, 0},
            .extent = drawExtent(),
        };
        vkCmdSetScissor(commandBuffer, 0, 1, &full_scissor);
    }

    // ------------ VkPipelineDepthStencilStateCreateInfo ------------
    /* VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE */
    vkCmdSetDepthTestEnable(commandBuffer, overlayDepthTestEnable);

    /* VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE */
    vkCmdSetDepthWriteEnable(commandBuffer, overlayDepthWriteEnable);

    /* VK_DYNAMIC_STATE_DEPTH_COMPARE_OP */
    vkCmdSetDepthCompareOp(commandBuffer, overlayDepthCompareOp);

    /* VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE */
    vkCmdSetStencilTestEnable(commandBuffer, overlayStencilTestEnable);

    /* VK_DYNAMIC_STATE_STENCIL_OP */
    vkCmdSetStencilOp(commandBuffer, VK_STENCIL_FACE_FRONT_BIT, overlayFailOp[0], overlayPassOp[0],
                      overlayDepthFailOp[0], overlayCompareOp[0]);
    vkCmdSetStencilOp(commandBuffer, VK_STENCIL_FACE_BACK_BIT, overlayFailOp[1], overlayPassOp[1],
                      overlayDepthFailOp[1], overlayCompareOp[1]);

    /* VK_DYNAMIC_STATE_STENCIL_REFERENCE */
    vkCmdSetStencilReference(commandBuffer, VK_STENCIL_FACE_FRONT_BIT, overlayReference[0]);
    vkCmdSetStencilReference(commandBuffer, VK_STENCIL_FACE_BACK_BIT, overlayReference[1]);

    /* VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK */
    vkCmdSetStencilCompareMask(commandBuffer, VK_STENCIL_FACE_FRONT_BIT, overlayCompareMask[0]);
    vkCmdSetStencilCompareMask(commandBuffer, VK_STENCIL_FACE_BACK_BIT, overlayCompareMask[1]);

    /* VK_DYNAMIC_STATE_STENCIL_WRITE_MASK */
    vkCmdSetStencilWriteMask(commandBuffer, VK_STENCIL_FACE_FRONT_BIT, overlayWriteMask[0]);
    vkCmdSetStencilWriteMask(commandBuffer, VK_STENCIL_FACE_BACK_BIT, overlayWriteMask[1]);

    // ------------ VkPipelineRasterizationStateCreateInfo ------------
    /* VK_DYNAMIC_STATE_CULL_MODE */
    vkCmdSetCullMode(commandBuffer, overlayCullMode);

    /* VK_DYNAMIC_STATE_FRONT_FACE */
    vkCmdSetFrontFace(commandBuffer, overlayFrontFace);

    /* VK_DYNAMIC_STATE_POLYGON_MODE_EXT */
    vkCmdSetPolygonModeEXT(commandBuffer, overlayPolygonMode);

    /* VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE */
    vkCmdSetDepthBiasEnable(commandBuffer, overlayDepthBiasEnable);

    /* VK_DYNAMIC_STATE_DEPTH_BIAS */
    vkCmdSetDepthBias(commandBuffer, overlayDepthBiasConstantFactor[overlayPolygonMode],
                      overlayDepthBiasClamp[overlayPolygonMode], overlayDepthBiasSlopeFactor[overlayPolygonMode]);

    /* VK_DYNAMIC_STATE_LINE_WIDTH */
    vkCmdSetLineWidth(commandBuffer, overlayLineWidth);

    // ------------ VkPipelineColorBlendAttachmentState / StateCreateInfo ------------
    /* VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT */
    syncColorAttachments();

    /* VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT */

    /* VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT */

    // ------------ VkPipelineColorBlendStateCreateInfo ------------
    /* VK_DYNAMIC_STATE_LOGIC_OP_EXT and VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT */
    // Only call these if extendedDynamicState2LogicOp feature is enabled
    if (context->device->hasExtendedDynamicState2LogicOp()) {
        vkCmdSetLogicOpEXT(commandBuffer, overlayColorLogicOp);
        vkCmdSetLogicOpEnableEXT(commandBuffer, overlayColorLogicOpEnable);
    }

    /* VK_DYNAMIC_STATE_BLEND_CONSTANTS */
    vkCmdSetBlendConstants(commandBuffer, overlayBlendConstants.data());
}

void UIModuleContext::syncFromContext(std::shared_ptr<UIModuleContext> other) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    copyPersistentStateFrom(*other);
    syncToCommandBuffer();
}

void UIModuleContext::copyPersistentStateFrom(const UIModuleContext &other) {
    overlayScissorEnabled = other.overlayScissorEnabled;
    overlayScissor = other.overlayScissor;
    overlayScissorGl = other.overlayScissorGl;
    overlayViewport = other.overlayViewport;
    overlayViewportGl = other.overlayViewportGl;

    // OpenGL keeps custom viewport/scissor state across a resize, but the default full-window
    // values follow the drawable extent. Distinguish those cases using the old target size.
    auto context = frameworkContext.lock();
    const uint32_t oldWidth = other.overlayDrawColorImage->width();
    const uint32_t oldHeight = other.overlayDrawColorImage->height();
    const uint32_t newWidth = context->swapchainImage->width();
    const uint32_t newHeight = context->swapchainImage->height();
    if (overlayViewport.x == 0.0f && overlayViewport.y == 0.0f &&
        overlayViewport.width == static_cast<float>(oldWidth) &&
        overlayViewport.height == static_cast<float>(oldHeight)) {
        overlayViewport.width = static_cast<float>(newWidth);
        overlayViewport.height = static_cast<float>(newHeight);
        overlayViewportGl = {0, 0, static_cast<int>(newWidth), static_cast<int>(newHeight)};
    }
    if (overlayScissor.offset.x == 0 && overlayScissor.offset.y == 0 &&
        overlayScissor.extent.width == oldWidth && overlayScissor.extent.height == oldHeight) {
        overlayScissor.extent = {.width = newWidth, .height = newHeight};
    }

    overlayBlendEnabled = other.overlayBlendEnabled;
    overlayColorBlendEquation = other.overlayColorBlendEquation;
    overlayColorWriteMask = other.overlayColorWriteMask;
    overlayColorLogicOpEnable = other.overlayColorLogicOpEnable;
    overlayColorLogicOp = other.overlayColorLogicOp;
    overlayBlendConstants = other.overlayBlendConstants;

    overlayDepthTestEnable = other.overlayDepthTestEnable;
    overlayDepthWriteEnable = other.overlayDepthWriteEnable;
    overlayDepthCompareOp = other.overlayDepthCompareOp;
    overlayStencilTestEnable = other.overlayStencilTestEnable;
    overlayFailOp = other.overlayFailOp;
    overlayPassOp = other.overlayPassOp;
    overlayDepthFailOp = other.overlayDepthFailOp;
    overlayCompareOp = other.overlayCompareOp;
    overlayReference = other.overlayReference;
    overlayCompareMask = other.overlayCompareMask;
    overlayWriteMask = other.overlayWriteMask;

    overlayCullMode = other.overlayCullMode;
    overlayFrontFace = other.overlayFrontFace;
    overlayPolygonMode = other.overlayPolygonMode;
    overlayDepthBiasEnable = other.overlayDepthBiasEnable;
    overlayDepthBiasConstantFactor = other.overlayDepthBiasConstantFactor;
    overlayDepthBiasClamp = other.overlayDepthBiasClamp;
    overlayDepthBiasSlopeFactor = other.overlayDepthBiasSlopeFactor;
    overlayLineWidth = other.overlayLineWidth;

    overlayClearColors = other.overlayClearColors;
    overlayClearDepth = other.overlayClearDepth;
    overlayClearStencil = other.overlayClearStencil;
}

// Outside a raster pass, the state is still observable through the shadow fields,
// but no draw can consume its transient values. Every draw/pass entry replays the
// full state through syncToCommandBuffer. Keep eager recording as a local reference.
bool UIModuleContext::deferIdleStateWrite() const {
    static const bool enabled = [] {
        const char *value = std::getenv("MCVR_IDLE_UI_STATE");
        return !value || std::string_view(value) != "0";
    }();
    return enabled && overlayMode == NONE;
}

void UIModuleContext::setOverlayScissorEnabled(bool enabled) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayScissorEnabled = enabled;
    if (deferIdleStateWrite()) return;
    if (overlayScissorEnabled) {
        vkCmdSetScissor(context->overlayCommandBuffer->vkCommandBuffer(), 0, 1, &overlayScissor);
    } else {
        VkRect2D full_scissor = {
            .offset = {0, 0},
            .extent = drawExtent(),
        };
        vkCmdSetScissor(context->overlayCommandBuffer->vkCommandBuffer(), 0, 1, &full_scissor);
    }
}

void UIModuleContext::setOverlayScissor(int x, int y, int width, int height) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    if (width < 0 || height < 0) throw std::invalid_argument("Negative scissor size");
    overlayScissorGl = {x, y, width, height};
    auto extent = drawExtent();
    const int right = std::clamp(static_cast<int64_t>(x) + width, int64_t(0), int64_t(extent.width));
    const int bottom = std::clamp(int64_t(extent.height) - y, int64_t(0), int64_t(extent.height));
    const int left = std::clamp<int64_t>(x, 0, extent.width);
    const int top = std::clamp(int64_t(extent.height) - y - height, int64_t(0), int64_t(extent.height));
    overlayScissor = {{left, top}, {static_cast<uint32_t>(std::max(0, right - left)),
                                  static_cast<uint32_t>(std::max(0, bottom - top))}};
    setOverlayScissorEnabled(overlayScissorEnabled);
}

void UIModuleContext::setOverlayViewport(int x, int y, int width, int height) {
    if (width < 0 || height < 0) throw std::invalid_argument("Negative viewport size");
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayViewportGl = {x, y, width, height};
    overlayViewport.x = x;
    overlayViewport.y = static_cast<float>(drawExtent().height) - y - height;
    overlayViewport.width = width;
    overlayViewport.height = height;
    if (deferIdleStateWrite()) return;
    vkCmdSetViewport(context->overlayCommandBuffer->vkCommandBuffer(), 0, 1, &overlayViewport);
}

void UIModuleContext::setOverlayBlendEnable(bool enable) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayBlendEnabled = enable;
    if (deferIdleStateWrite()) return;
    syncColorAttachments();
}

void UIModuleContext::setOverlayColorBlendConstants(float const1, float const2, float const3, float const4) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayBlendConstants[0] = const1;
    overlayBlendConstants[1] = const2;
    overlayBlendConstants[2] = const3;
    overlayBlendConstants[3] = const4;
    if (deferIdleStateWrite()) return;
    vkCmdSetBlendConstants(context->overlayCommandBuffer->vkCommandBuffer(), overlayBlendConstants.data());
}

void UIModuleContext::setOverlayColorLogicOpEnable(bool enable) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayColorLogicOpEnable = enable;
    if (deferIdleStateWrite()) return;
    if (context->device->hasExtendedDynamicState2LogicOp()) {
        vkCmdSetLogicOpEnableEXT(context->overlayCommandBuffer->vkCommandBuffer(), overlayColorLogicOpEnable);
    }
}

void UIModuleContext::setOverlayBlendFuncSeparate(int srcColorBlendFactor,
                                                  int srcAlphaBlendFactor,
                                                  int dstColorBlendFactor,
                                                  int dstAlphaBlendFactor) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayColorBlendEquation.srcColorBlendFactor = static_cast<VkBlendFactor>(srcColorBlendFactor);
    overlayColorBlendEquation.srcAlphaBlendFactor = static_cast<VkBlendFactor>(srcAlphaBlendFactor);
    overlayColorBlendEquation.dstColorBlendFactor = static_cast<VkBlendFactor>(dstColorBlendFactor);
    overlayColorBlendEquation.dstAlphaBlendFactor = static_cast<VkBlendFactor>(dstAlphaBlendFactor);
    if (deferIdleStateWrite()) return;
    syncColorAttachments();
}

void UIModuleContext::setOverlayBlendOpSeparate(int colorBlendOp, int alphaBlendOp) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayColorBlendEquation.colorBlendOp = static_cast<VkBlendOp>(colorBlendOp);
    overlayColorBlendEquation.alphaBlendOp = static_cast<VkBlendOp>(alphaBlendOp);
    if (deferIdleStateWrite()) return;
    syncColorAttachments();
}

void UIModuleContext::setOverlayColorWriteMask(int colorWriteMask) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayColorWriteMask = colorWriteMask;
    if (deferIdleStateWrite()) return;
    syncColorAttachments();
}

void UIModuleContext::setOverlayColorLogicOp(int colorLogicOp) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayColorLogicOp = static_cast<VkLogicOp>(colorLogicOp);
    if (deferIdleStateWrite()) return;
    if (context->device->hasExtendedDynamicState2LogicOp()) {
        vkCmdSetLogicOpEXT(context->overlayCommandBuffer->vkCommandBuffer(), overlayColorLogicOp);
    }
}

void UIModuleContext::setOverlayDepthTestEnable(bool enable) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayDepthTestEnable = enable;
    if (deferIdleStateWrite()) return;
    vkCmdSetDepthTestEnable(context->overlayCommandBuffer->vkCommandBuffer(), overlayDepthTestEnable);
}

void UIModuleContext::setOverlayDepthWriteEnable(bool enable) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayDepthWriteEnable = enable;
    if (deferIdleStateWrite()) return;
    vkCmdSetDepthWriteEnable(context->overlayCommandBuffer->vkCommandBuffer(), overlayDepthWriteEnable);
}

void UIModuleContext::setOverlayStencilTestEnable(bool enable) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayStencilTestEnable = enable;
    if (deferIdleStateWrite()) return;
    vkCmdSetStencilTestEnable(context->overlayCommandBuffer->vkCommandBuffer(), overlayStencilTestEnable);
}

void UIModuleContext::setOverlayDepthCompareOp(int depthCompareOp) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayDepthCompareOp = static_cast<VkCompareOp>(depthCompareOp);
    if (deferIdleStateWrite()) return;
    vkCmdSetDepthCompareOp(context->overlayCommandBuffer->vkCommandBuffer(), overlayDepthCompareOp);
}

void UIModuleContext::setOverlayStencilFrontFunc(int compareOp, int reference, int compareMask) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayCompareOp[0] = static_cast<VkCompareOp>(compareOp);
    overlayReference[0] = reference;
    overlayCompareMask[0] = compareMask;
    if (deferIdleStateWrite()) return;
    vkCmdSetStencilOp(context->overlayCommandBuffer->vkCommandBuffer(), VK_STENCIL_FACE_FRONT_BIT, overlayFailOp[0],
                      overlayPassOp[0], overlayDepthFailOp[0], overlayCompareOp[0]);
    vkCmdSetStencilReference(context->overlayCommandBuffer->vkCommandBuffer(), VK_STENCIL_FACE_FRONT_BIT,
                             overlayReference[0]);
    vkCmdSetStencilCompareMask(context->overlayCommandBuffer->vkCommandBuffer(), VK_STENCIL_FACE_FRONT_BIT,
                               overlayCompareMask[0]);
}

void UIModuleContext::setOverlayStencilBackFunc(int compareOp, int reference, int compareMask) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayCompareOp[1] = static_cast<VkCompareOp>(compareOp);
    overlayReference[1] = reference;
    overlayCompareMask[1] = compareMask;
    if (deferIdleStateWrite()) return;
    vkCmdSetStencilOp(context->overlayCommandBuffer->vkCommandBuffer(), VK_STENCIL_FACE_BACK_BIT, overlayFailOp[1],
                      overlayPassOp[1], overlayDepthFailOp[1], overlayCompareOp[1]);
    vkCmdSetStencilReference(context->overlayCommandBuffer->vkCommandBuffer(), VK_STENCIL_FACE_BACK_BIT,
                             overlayReference[1]);
    vkCmdSetStencilCompareMask(context->overlayCommandBuffer->vkCommandBuffer(), VK_STENCIL_FACE_BACK_BIT,
                               overlayCompareMask[1]);
}

void UIModuleContext::setOverlayStencilFrontOp(int failOp, int depthFailOp, int passOp) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayFailOp[0] = static_cast<VkStencilOp>(failOp);
    overlayDepthFailOp[0] = static_cast<VkStencilOp>(depthFailOp);
    overlayPassOp[0] = static_cast<VkStencilOp>(passOp);
    if (deferIdleStateWrite()) return;
    vkCmdSetStencilOp(context->overlayCommandBuffer->vkCommandBuffer(), VK_STENCIL_FACE_FRONT_BIT, overlayFailOp[0],
                      overlayPassOp[0], overlayDepthFailOp[0], overlayCompareOp[0]);
}

void UIModuleContext::setOverlayStencilBackOp(int failOp, int depthFailOp, int passOp) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayFailOp[1] = static_cast<VkStencilOp>(failOp);
    overlayDepthFailOp[1] = static_cast<VkStencilOp>(depthFailOp);
    overlayPassOp[1] = static_cast<VkStencilOp>(passOp);
    if (deferIdleStateWrite()) return;
    vkCmdSetStencilOp(context->overlayCommandBuffer->vkCommandBuffer(), VK_STENCIL_FACE_BACK_BIT, overlayFailOp[1],
                      overlayPassOp[1], overlayDepthFailOp[1], overlayCompareOp[1]);
}

void UIModuleContext::setOverlayStencilFrontWriteMask(int writeMask) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayWriteMask[0] = writeMask;
    if (deferIdleStateWrite()) return;
    vkCmdSetStencilWriteMask(context->overlayCommandBuffer->vkCommandBuffer(), VK_STENCIL_FACE_FRONT_BIT,
                             overlayWriteMask[0]);
}

void UIModuleContext::setOverlayStencilBackWriteMask(int writeMask) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayWriteMask[1] = writeMask;
    if (deferIdleStateWrite()) return;
    vkCmdSetStencilWriteMask(context->overlayCommandBuffer->vkCommandBuffer(), VK_STENCIL_FACE_BACK_BIT,
                             overlayWriteMask[1]);
}

void UIModuleContext::setOverlayLineWidth(float lineWidth) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayLineWidth = lineWidth;
    if (deferIdleStateWrite()) return;
    vkCmdSetLineWidth(context->overlayCommandBuffer->vkCommandBuffer(), overlayLineWidth);
}

void UIModuleContext::setOverlayPolygonMode(int polygonMode) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayPolygonMode = static_cast<VkPolygonMode>(polygonMode);
    if (deferIdleStateWrite()) return;
    vkCmdSetPolygonModeEXT(context->overlayCommandBuffer->vkCommandBuffer(), overlayPolygonMode);
    setOverlayDepthBiasEnable(overlayPolygonMode, overlayDepthBiasEnable);
}

void UIModuleContext::setOverlayCullMode(int cullMode) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayCullMode = cullMode;
    if (deferIdleStateWrite()) return;
    vkCmdSetCullMode(context->overlayCommandBuffer->vkCommandBuffer(), overlayCullMode);
}

void UIModuleContext::setOverlayFrontFace(int frontFace) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayFrontFace = static_cast<VkFrontFace>(frontFace);
    if (deferIdleStateWrite()) return;
    vkCmdSetFrontFace(context->overlayCommandBuffer->vkCommandBuffer(), overlayFrontFace);
}

void UIModuleContext::setOverlayDepthBiasEnable(int polygonMode, bool enable) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayDepthBiasEnable = enable;
    if (deferIdleStateWrite()) return;
    if (overlayDepthBiasEnable)
        vkCmdSetDepthBias(context->overlayCommandBuffer->vkCommandBuffer(),
                          overlayDepthBiasConstantFactor[overlayPolygonMode], overlayDepthBiasClamp[overlayPolygonMode],
                          overlayDepthBiasSlopeFactor[overlayPolygonMode]);
    vkCmdSetDepthBiasEnable(context->overlayCommandBuffer->vkCommandBuffer(), overlayDepthBiasEnable);
}

void UIModuleContext::setOverlayDepthBias(float depthBiasSlopeFactor, float depthBiasConstantFactor) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayDepthBiasSlopeFactor[overlayPolygonMode] = depthBiasSlopeFactor;
    overlayDepthBiasConstantFactor[overlayPolygonMode] = depthBiasConstantFactor;
    setOverlayDepthBiasEnable(overlayPolygonMode, overlayDepthBiasEnable);
}

void UIModuleContext::setOverlayClearColor(float red, float green, float blue, float alpha) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayClearColors[0] = red;
    overlayClearColors[1] = green;
    overlayClearColors[2] = blue;
    overlayClearColors[3] = alpha;
}

void UIModuleContext::setOverlayClearDepth(double depth) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayClearDepth = static_cast<float>(std::clamp(depth, 0.0, 1.0));
}

void UIModuleContext::setOverlayClearStencil(int stencil) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayClearStencil = stencil;
}

void UIModuleContext::switchOverlayDraw() {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto module = uiModule.lock();

    if (!framework->isRunning()) return;
    if (switchFramebufferDraw()) return;
    if (overlayMode == FRAMEBUFFER_DRAW) endFramebufferDraw();
    if (overlayMode == DIAGRAM_DRAW) return;

    auto mainQueueIndex = context->physicalDevice->mainQueueIndex();

    if (overlayMode == POST) {
        context->overlayCommandBuffer->endRenderPass();
        overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    if (overlayMode == NONE || overlayMode == POST) {
        context->overlayCommandBuffer->barriersBufferImage(
            {}, {{
                     .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     .oldLayout = overlayDrawColorImage->imageLayout(),
#ifdef USE_AMD
                     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
#else
                     .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
#endif
                     .srcQueueFamilyIndex = mainQueueIndex,
                     .dstQueueFamilyIndex = mainQueueIndex,
                     .image = overlayDrawColorImage,
                     .subresourceRange = vk::wholeColorSubresourceRange,
                 },
                 {
                     .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     .oldLayout = overlayDrawDepthStencilImage->imageLayout(),
                     .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     .srcQueueFamilyIndex = mainQueueIndex,
                     .dstQueueFamilyIndex = mainQueueIndex,
                     .image = overlayDrawDepthStencilImage,
                     .subresourceRange = overlayDrawDepthStencilImage->fullSubresourceRange(),
                 }});
#ifdef USE_AMD
        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
#else
        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
#endif
        overlayDrawDepthStencilImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        context->overlayCommandBuffer->beginRenderPass({
            .renderPass = module->overlayDrawRenderPass_,
            .framebuffer = overlayDrawFramebuffer,
            .renderAreaExtent = {overlayDrawColorImage->width(), overlayDrawColorImage->height()},
            .clearValues = {{.color = {0.1f, 0.1f, 0.1f, 1.0f}}, {.depthStencil = {.depth = 1.0f}}},
        });

        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        overlayDrawDepthStencilImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        syncToCommandBuffer();
    }

    overlayMode = DRAW;
}

void UIModuleContext::beginDiagram(int x, int y, int width, int height) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto module = uiModule.lock();

    if (!framework->isRunning() || width <= 0 || height <= 0) return;

    if (overlayMode == DRAW) {
        context->overlayCommandBuffer->endRenderPass();
#ifdef USE_AMD
        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
#else
        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
#endif
        overlayDrawDepthStencilImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    } else if (overlayMode == POST) {
        context->overlayCommandBuffer->endRenderPass();
        overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else if (overlayMode == DIAGRAM_DRAW) {
        context->overlayCommandBuffer->endRenderPass();
        diagramDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        diagramDrawDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    overlayMode = NONE;

    auto mainQueueIndex = context->physicalDevice->mainQueueIndex();
    context->overlayCommandBuffer->barriersBufferImage(
        {}, {{
                 .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 .oldLayout = diagramDrawColorImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = diagramDrawColorImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 .oldLayout = diagramDrawDepthImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = diagramDrawDepthImage,
                 .subresourceRange = vk::wholeDepthSubresourceRange,
             }});
    diagramDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    diagramDrawDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    context->overlayCommandBuffer->beginRenderPass({
        .renderPass = module->diagramDrawRenderPass_,
        .framebuffer = diagramDrawFramebuffer,
        .renderAreaExtent = {diagramDrawColorImage->width(), diagramDrawColorImage->height()},
        .clearValues = {{.color = {0.0f, 0.0f, 0.0f, 0.0f}}, {.depthStencil = {.depth = 1.0f}}},
    });

    VkClearAttachment attachments[2]{};
    attachments[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachments[0].colorAttachment = 0;
    attachments[0].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    attachments[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    attachments[1].clearValue.depthStencil = {.depth = 1.0f, .stencil = 0};
    VkClearRect clearRect{};
    clearRect.rect.offset = {0, 0};
    clearRect.rect.extent = framework->swapchain()->vkExtent();
    clearRect.baseArrayLayer = 0;
    clearRect.layerCount = 1;
    vkCmdClearAttachments(context->overlayCommandBuffer->vkCommandBuffer(), 2, attachments, 1, &clearRect);

    if (!diagramStateSaved) {
        diagramStateSaved = true;
        savedOverlayScissorEnabled = overlayScissorEnabled;
        savedOverlayScissor = overlayScissor;
        savedOverlayScissorGl = overlayScissorGl;
        savedOverlayViewport = overlayViewport;
        savedOverlayViewportGl = overlayViewportGl;
        savedOverlayDepthTestEnable = overlayDepthTestEnable;
        savedOverlayDepthWriteEnable = overlayDepthWriteEnable;
        savedOverlayDepthCompareOp = overlayDepthCompareOp;
    }

    VkExtent2D extent = framework->swapchain()->vkExtent();
    int clampedX = std::clamp(x, 0, static_cast<int>(extent.width));
    int clampedY = std::clamp(y, 0, static_cast<int>(extent.height));
    int clampedWidth = std::clamp(width, 0, static_cast<int>(extent.width) - clampedX);
    int clampedHeight = std::clamp(height, 0, static_cast<int>(extent.height) - clampedY);
    overlayViewportGl = {clampedX, static_cast<int>(extent.height) - clampedY - clampedHeight, clampedWidth, clampedHeight};
    overlayViewport = {
        .x = static_cast<float>(clampedX),
        .y = static_cast<float>(clampedY),
        .width = static_cast<float>(clampedWidth),
        .height = static_cast<float>(clampedHeight),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    overlayScissorEnabled = true;
    overlayScissor = {
        .offset = {clampedX, clampedY},
        .extent = {static_cast<uint32_t>(clampedWidth), static_cast<uint32_t>(clampedHeight)},
    };
    overlayDepthTestEnable = true;
    overlayDepthWriteEnable = true;
    overlayDepthCompareOp = VK_COMPARE_OP_LESS;
    overlayMode = DIAGRAM_DRAW;
    syncToCommandBuffer();
}

void UIModuleContext::switchOverlayPost() {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto module = uiModule.lock();
    module->prepareOverlayDescriptorTable(context->frameIndex);

    if (!framework->isRunning()) return;

    auto mainQueueIndex = context->physicalDevice->mainQueueIndex();

    if (overlayMode == DRAW) {
        context->overlayCommandBuffer->endRenderPass();
#ifdef USE_AMD
        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

#else
        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
#endif
        overlayDrawDepthStencilImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    if (overlayMode == NONE || overlayMode == DRAW) {
        context->overlayCommandBuffer->barriersBufferImage(
            {}, {{
                      .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                      .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     .oldLayout = overlayPostColorImage->imageLayout(),
                    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     .srcQueueFamilyIndex = mainQueueIndex,
                     .dstQueueFamilyIndex = mainQueueIndex,
                     .image = overlayPostColorImage,
                     .subresourceRange = vk::wholeColorSubresourceRange,
                 },
                 {
                      .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                      .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      .oldLayout = overlayDrawColorImage->imageLayout(),
                     .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     .srcQueueFamilyIndex = mainQueueIndex,
                     .dstQueueFamilyIndex = mainQueueIndex,
                     .image = overlayDrawColorImage,
                     .subresourceRange = vk::wholeColorSubresourceRange,
                 }});
        overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        context->overlayCommandBuffer->beginRenderPass({
            .renderPass = module->overlayPostRenderPass_,
            .framebuffer = overlayPostFramebuffer,
            .renderAreaExtent = {overlayPostColorImage->width(), overlayPostColorImage->height()},
            .clearValues = {{.color = {0.1f, 0.1f, 0.1f, 1.0f}}, {.depthStencil = {.depth = 1.0f}}},
        });
        overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    if (overlayMode == NONE) syncToCommandBuffer();
    overlayMode = POST;
}

void UIModuleContext::clearOverlayEntireColorAttachment() {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    switchOverlayDraw();
    if (overlayColorWriteMask == 0) return;
    if (overlayColorWriteMask != (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)) {
        clearMaskedFramebuffer(true, false, false);
        return;
    }

    VkClearAttachment clearAttachment{};
    clearAttachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearAttachment.colorAttachment = 0;
    clearAttachment.clearValue.color.float32[0] = overlayClearColors[0];
    clearAttachment.clearValue.color.float32[1] = overlayClearColors[1];
    clearAttachment.clearValue.color.float32[2] = overlayClearColors[2];
    clearAttachment.clearValue.color.float32[3] = overlayClearColors[3];

    VkClearRect clearRect{};
    clearRect.rect.offset = {0, 0};
    clearRect.rect.extent = drawExtent();
    if (overlayScissorEnabled) clearRect.rect = overlayScissor;
    if (clearRect.rect.extent.width == 0 || clearRect.rect.extent.height == 0) return;
    clearRect.baseArrayLayer = 0;
    clearRect.layerCount = 1;

    if (overlayColorWriteMask == 0) return;
    auto target = framebufferSnapshot(mcvr::framebuffer::DRAW_FRAMEBUFFER);
    std::vector<VkClearAttachment> clears;
    for (uint32_t slot = 0; slot < target.drawColors.size(); ++slot) {
        if (!target.drawColors[slot]) continue;
        clearAttachment.colorAttachment = slot;
        clears.push_back(clearAttachment);
    }
    if (!clears.empty()) vkCmdClearAttachments(context->overlayCommandBuffer->vkCommandBuffer(),
        static_cast<uint32_t>(clears.size()), clears.data(), 1, &clearRect);
}

void UIModuleContext::clearOverlayEntireDepthStencilAttachment(int aspectMask) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    switchOverlayDraw();

    VkClearAttachment clearAttachment{};
    if (!overlayDepthWriteEnable) aspectMask &= ~VK_IMAGE_ASPECT_DEPTH_BIT;
    auto target = framebufferSnapshot(mcvr::framebuffer::DRAW_FRAMEBUFFER);
    if (!target.depthStencil) return;
    aspectMask &= target.depthStencil->aspectMask;
    if (aspectMask == 0) return;
    if ((aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && (overlayWriteMask[0] & 0xffu) != 0xffu) {
        clearMaskedFramebuffer(false, (aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, true);
        return;
    }
    clearAttachment.aspectMask = aspectMask;
    clearAttachment.clearValue.depthStencil.depth = overlayClearDepth;
    clearAttachment.clearValue.depthStencil.stencil = overlayClearStencil;

    VkClearRect clearRect{};
    clearRect.rect.offset = {0, 0};
    clearRect.rect.extent = drawExtent();
    if (overlayScissorEnabled) clearRect.rect = overlayScissor;
    if (clearRect.rect.extent.width == 0 || clearRect.rect.extent.height == 0) return;
    clearRect.baseArrayLayer = 0;
    clearRect.layerCount = 1;

    vkCmdClearAttachments(context->overlayCommandBuffer->vkCommandBuffer(), 1, &clearAttachment, 1, &clearRect);
}

void UIModuleContext::drawIndexed(std::shared_ptr<vk::DeviceLocalBuffer> vertexBuffer,
                                  std::shared_ptr<vk::DeviceLocalBuffer> indexBuffer,
                                  std::shared_ptr<vk::DeviceLocalBuffer> patchIndexBuffer,
                                  uint32_t shaderId,
                                  uint32_t uniformOffset,
                                  uint32_t indexCount,
                                  uint32_t patchIndexCount,
                                  VkIndexType indexType) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto module = uiModule.lock();

    if (!framework->isRunning()) return;

    module->prepareOverlayDescriptorTable(context->frameIndex);
    switchOverlayDraw();
    if (overlayMode != FRAMEBUFFER_DRAW && (overlayBlendEnabled || overlayColorLogicOpEnable) &&
        !mcvr::ui::sourceOverColor(overlayColorBlendEquation.srcColorBlendFactor,
            overlayColorBlendEquation.dstColorBlendFactor, overlayColorBlendEquation.colorBlendOp,
            overlayColorLogicOpEnable) &&
        !mcvr::ui::backgroundModulation(overlayColorBlendEquation.srcColorBlendFactor,
            overlayColorBlendEquation.dstColorBlendFactor, overlayColorBlendEquation.colorBlendOp,
            overlayColorLogicOpEnable)) ++context->fgBackgroundDependentDraws;

    prepareBackgroundModulation();
    auto &shaderInfo = module->overlayDrawShaderInfo(shaderId);
    auto graphicsPipeline = overlayMode == FRAMEBUFFER_DRAW ? framebufferPipeline(shaderId) : shaderInfo.pipeline;
    vkCmdBindPipeline(context->overlayCommandBuffer->vkCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                      graphicsPipeline->vkPipeline());

    // A draw with blending disabled replaces RGB, irrespective of texture alpha.
    // Keep discarded/depth-rejected fragments untouched; off-screen alpha is original.
    const uint32_t opaqueCoverage = overlayMode != FRAMEBUFFER_DRAW && !overlayBlendEnabled &&
        !overlayColorLogicOpEnable;
    vkCmdPushConstants(context->overlayCommandBuffer->vkCommandBuffer(),
        overlayDescriptorTable->vkPipelineLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(opaqueCoverage), &opaqueCoverage);

    uint32_t dynamicOffsets[] = {uniformOffset, 0};
    vkCmdBindDescriptorSets(context->overlayCommandBuffer->vkCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            overlayDescriptorTable->vkPipelineLayout(), 0, overlayDescriptorTable->descriptorSet().size(),
                            overlayDescriptorTable->descriptorSet().data(), 2, dynamicOffsets);

    context->overlayCommandBuffer->bindVertexBuffers(vertexBuffer);
    const auto recordAndDraw = [&](const std::shared_ptr<vk::DeviceLocalBuffer> &drawIndexBuffer,
                                   uint32_t drawIndexCount,
                                   uint32_t drawPatchIndexCount) {
        context->overlayCommandBuffer->bindIndexBuffer(drawIndexBuffer, indexType);
        if (mcvr::diagnostics::drawStateTraceEnabled()) {
            std::vector<uint64_t> descriptorSets;
            const auto &sets = overlayDescriptorTable->descriptorSet();
            descriptorSets.reserve(sets.size());
            for (VkDescriptorSet set : sets) {
                descriptorSets.push_back(mcvr::diagnostics::handleValue(set));
            }
            const auto &commandBuffer = context->overlayCommandBuffer->vkCommandBuffer();
            mcvr::diagnostics::recordDraw(
                "ShaderProxy.draw", context->frameIndex, context->frameSubmitted,
                static_cast<uint32_t>(overlayMode), shaderId, shaderInfo.key,
                mcvr::diagnostics::handleValue(commandBuffer),
                mcvr::diagnostics::handleValue(graphicsPipeline->vkPipeline()),
                mcvr::diagnostics::handleValue(overlayDescriptorTable->vkPipelineLayout()),
                reinterpret_cast<uint64_t>(overlayDescriptorTable.get()), descriptorSets,
                reinterpret_cast<uint64_t>(vertexBuffer.get()),
                mcvr::diagnostics::handleValue(vertexBuffer->vkBuffer()), vertexBuffer->size(),
                reinterpret_cast<uint64_t>(drawIndexBuffer.get()),
                mcvr::diagnostics::handleValue(drawIndexBuffer->vkBuffer()), drawIndexBuffer->size(),
                (shaderInfo.patchControlPoints == 0 || patchIndexBuffer == nullptr)
                    ? 0 : reinterpret_cast<uint64_t>(patchIndexBuffer.get()),
                (shaderInfo.patchControlPoints == 0 || patchIndexBuffer == nullptr)
                    ? 0 : mcvr::diagnostics::handleValue(patchIndexBuffer->vkBuffer()),
                (shaderInfo.patchControlPoints == 0 || patchIndexBuffer == nullptr)
                    ? 0 : patchIndexBuffer->size(),
                uniformOffset, drawIndexCount, drawPatchIndexCount, 1, static_cast<uint32_t>(indexType));
        }
        context->overlayCommandBuffer->drawIndexed(drawIndexCount, 1);
        if (mcvr::diagnostics::drawStateTraceEnabled()) {
            mcvr::diagnostics::recordFrame(
                "DRAW_RETURN", context->frameIndex, context->frameSubmitted, 0,
                mcvr::diagnostics::handleValue(context->overlayCommandBuffer->vkCommandBuffer()));
        }
    };
    if (shaderInfo.patchControlPoints != 0) {
        if (!patchIndexBuffer || patchIndexCount == 0
            || patchIndexCount % shaderInfo.patchControlPoints != 0)
            throw std::runtime_error("Tessellation draw requires complete four-control-point indices");
        recordAndDraw(patchIndexBuffer, patchIndexCount, patchIndexCount);
    } else {
        recordAndDraw(indexBuffer, indexCount, 0);
    }
    mirrorBackgroundModulation([&] {
        drawIndexed(vertexBuffer, indexBuffer, patchIndexBuffer, shaderId, uniformOffset,
            indexCount, patchIndexCount, indexType);
    });
}

void UIModuleContext::prepareBackgroundModulation() {
    if (mirroringBackgroundModulation) return;
    backgroundModulationPrepared = false;
    if (overlayMode != DRAW || !overlayBlendEnabled ||
        !mcvr::ui::backgroundModulation(overlayColorBlendEquation.srcColorBlendFactor,
            overlayColorBlendEquation.dstColorBlendFactor, overlayColorBlendEquation.colorBlendOp,
            overlayColorLogicOpEnable)) return;
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    if (!framework->frameGenerationHudless(*context)) return;
    end();
    auto depth = overlayDrawDepthStencilImage;
    const auto range = vk::formatSubresourceRange(depth->vkFormat(),1,1,1);
    if (!hudlessModulationDepth) {
        hudlessModulationDepth = vk::DeviceLocalImage::create(framework->device(),framework->vma(),false,
            depth->width(),depth->height(),1,depth->vkFormat(),
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (range.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image=hudlessModulationDepth->vkImage();view.viewType=VK_IMAGE_VIEW_TYPE_2D;
            view.format=depth->vkFormat();view.subresourceRange=range;
            hudlessModulationDepth->addImageView(view);hudlessModulationDepthView=1;
        }
    }
    auto cmd = context->overlayCommandBuffer;
    const auto transition = [&](const auto &image, VkImageLayout layout) {
        cmd->barriersBufferImage({},{{.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout=image->imageLayout(),.newLayout=layout,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=image,.subresourceRange=range}});
        image->imageLayout()=layout;
    };
    transition(depth,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(hudlessModulationDepth,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (auto aspect : {VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_ASPECT_STENCIL_BIT}) {
        if (!(range.aspectMask & aspect)) continue;
        VkImageCopy copy{};copy.srcSubresource={static_cast<VkImageAspectFlags>(aspect),0,0,1};
        copy.dstSubresource=copy.srcSubresource;copy.extent={depth->width(),depth->height(),1};
        vkCmdCopyImage(cmd->vkCommandBuffer(),depth->vkImage(),depth->imageLayout(),
            hudlessModulationDepth->vkImage(),hudlessModulationDepth->imageLayout(),1,&copy);
    }
    transition(depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    transition(hudlessModulationDepth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    backgroundModulationPrepared = true;
    switchOverlayDraw();
}

void UIModuleContext::mirrorBackgroundModulation(const std::function<void()> &draw) {
    // For an affine background effect T(D)=b+m*D, replay T(H) and keep A:
    // T(U+(1-A)*H) = (m*U+b*A) + (1-A)*T(H).
    // This includes the vanilla crosshair S+(1-2*S)*D. RGB draw order is unchanged.
    if (mirroringBackgroundModulation || !backgroundModulationPrepared) return;
    backgroundModulationPrepared = false;
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto image = framework->frameGenerationHudless(*context);
    if (!image) return;
    ++context->fgAffineBackgroundDraws;
    auto module = uiModule.lock();
    if (hudlessModulationImage != image) {
        framework->frameResourceRetainer().retain(hudlessModulationFramebuffer);
        hudlessModulationImage = image;
        hudlessModulationFramebuffer = vk::FramebufferBuilder{}.beginAttachment()
            .defineAttachment(image)
            .defineAttachment(hudlessModulationDepth, hudlessModulationDepthView)
            .endAttachment().build(framework->device(), module->overlayDrawRenderPass_);
    }
    end();
    auto originalImage = overlayDrawColorImage;
    auto originalFramebuffer = overlayDrawFramebuffer;
    auto originalDepth = overlayDrawDepthStencilImage;
    overlayDrawColorImage = image;
    overlayDrawDepthStencilImage = hudlessModulationDepth;
    overlayDrawFramebuffer = hudlessModulationFramebuffer;
    mirroringBackgroundModulation = true;
    try { draw(); end(); }
    catch (...) {
        overlayDrawColorImage = originalImage; overlayDrawFramebuffer = originalFramebuffer;
        overlayDrawDepthStencilImage = originalDepth;
        mirroringBackgroundModulation = false;
        throw;
    }
    overlayDrawColorImage = originalImage; overlayDrawFramebuffer = originalFramebuffer;
    overlayDrawDepthStencilImage = originalDepth;
    mirroringBackgroundModulation = false;
    framework->frameResourceRetainer().retain(hudlessModulationFramebuffer);
}

void UIModuleContext::drawCustomVertexArray(
    const std::vector<CustomVertexBufferBinding> &vertexBuffers,
    std::shared_ptr<vk::DeviceLocalBuffer> indexBuffer,
    std::shared_ptr<vk::DeviceLocalBuffer> indirectBuffer,
    uint32_t shaderId, uint32_t uniformOffset, uint32_t indexCount,
    uint32_t instanceCount, VkIndexType indexType, VkDeviceSize indirectOffset,
    uint32_t indirectDrawCount, uint32_t indirectStride) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto module = uiModule.lock();
    if (!framework->isRunning()) return;
    auto &shaderInfo = module->overlayDrawShaderInfo(shaderId);
    if (!shaderInfo.customVertexLayout)
        throw std::runtime_error("Custom VertexArray requires a custom-layout shader");
    if (!indexBuffer) throw std::runtime_error("Custom VertexArray has no index buffer");

    module->prepareOverlayDescriptorTable(context->frameIndex);
    switchOverlayDraw();
    if (overlayMode != FRAMEBUFFER_DRAW && (overlayBlendEnabled || overlayColorLogicOpEnable) &&
        !mcvr::ui::sourceOverColor(overlayColorBlendEquation.srcColorBlendFactor,
            overlayColorBlendEquation.dstColorBlendFactor, overlayColorBlendEquation.colorBlendOp,
            overlayColorLogicOpEnable) &&
        !mcvr::ui::backgroundModulation(overlayColorBlendEquation.srcColorBlendFactor,
            overlayColorBlendEquation.dstColorBlendFactor, overlayColorBlendEquation.colorBlendOp,
            overlayColorLogicOpEnable)) ++context->fgBackgroundDependentDraws;
    prepareBackgroundModulation();
    VkCommandBuffer command = context->overlayCommandBuffer->vkCommandBuffer();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        (overlayMode == FRAMEBUFFER_DRAW ? framebufferPipeline(shaderId) : shaderInfo.pipeline)->vkPipeline());
    // A draw with blending disabled replaces RGB, irrespective of texture alpha.
    // Keep discarded/depth-rejected fragments untouched; off-screen alpha is original.
    const uint32_t opaqueCoverage = overlayMode != FRAMEBUFFER_DRAW && !overlayBlendEnabled &&
        !overlayColorLogicOpEnable;
    vkCmdPushConstants(context->overlayCommandBuffer->vkCommandBuffer(),
        overlayDescriptorTable->vkPipelineLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(opaqueCoverage), &opaqueCoverage);

    uint32_t dynamicOffsets[] = {uniformOffset, 0};
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        overlayDescriptorTable->vkPipelineLayout(), 0,
        static_cast<uint32_t>(overlayDescriptorTable->descriptorSet().size()),
        overlayDescriptorTable->descriptorSet().data(), 2, dynamicOffsets);
    for (const auto &binding : vertexBuffers) {
        if (!binding.buffer) throw std::runtime_error("Custom VertexArray binding has no buffer");
        VkBuffer handle = binding.buffer->vkBuffer();
        vkCmdBindVertexBuffers(command, binding.binding, 1, &handle, &binding.offset);
    }
    vkCmdBindIndexBuffer(command, indexBuffer->vkBuffer(), 0, indexType);
    if (indirectBuffer) {
        if (indirectDrawCount == 0) return;
        const uint32_t stride = indirectStride == 0 ? sizeof(VkDrawIndexedIndirectCommand)
                                                    : indirectStride;
        if (stride < sizeof(VkDrawIndexedIndirectCommand))
            throw std::runtime_error("Indirect indexed draw stride is too small");
        vkCmdDrawIndexedIndirect(command, indirectBuffer->vkBuffer(), indirectOffset,
            indirectDrawCount, stride);
    } else if (indexCount != 0 && instanceCount != 0) {
        vkCmdDrawIndexed(command, indexCount, instanceCount, 0, 0, 0);
    }
    mirrorBackgroundModulation([&] {
        drawCustomVertexArray(vertexBuffers, indexBuffer, indirectBuffer, shaderId, uniformOffset,
            indexCount, instanceCount, indexType, indirectOffset, indirectDrawCount, indirectStride);
    });
}

void UIModuleContext::postOverlay(OverlayPostPipelineType type, uint32_t uniformOffset) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto module = uiModule.lock();

    if (!framework->isRunning()) return;

    if (type == BLUR) {
        end();
        // The weighted background needs coverage BEFORE this pass blurs it.
        framework->blurFrameGenerationHudless(*context,
            Renderer::instance().buffers()->recordedOverlayPostUniform(uniformOffset), overlayDrawColorImage);
    }

    switchOverlayPost();

    vkCmdBindPipeline(context->overlayCommandBuffer->vkCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                      module->overlayPostPipelines_.at(type)->vkPipeline());
    uint32_t dynamicOffsets[] = {0, uniformOffset};
    vkCmdBindDescriptorSets(context->overlayCommandBuffer->vkCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            overlayDescriptorTable->vkPipelineLayout(), 0,
                            overlayDescriptorTable->descriptorSet().size(),
                            overlayDescriptorTable->descriptorSet().data(), 2, dynamicOffsets);

    context->overlayCommandBuffer->draw(3, 1);

    context->overlayCommandBuffer->endRenderPass();
        overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    overlayMode = NONE;

    auto mainQueueIndex = context->physicalDevice->mainQueueIndex();
    context->overlayCommandBuffer->barriersBufferImage(
        {}, {
                {
                    .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .oldLayout = overlayPostColorImage->imageLayout(),
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .srcQueueFamilyIndex = mainQueueIndex,
                    .dstQueueFamilyIndex = mainQueueIndex,
                    .image = overlayPostColorImage,
                    .subresourceRange = vk::wholeColorSubresourceRange,
                },
                {
                    .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .oldLayout = overlayDrawColorImage->imageLayout(),
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = mainQueueIndex,
                    .dstQueueFamilyIndex = mainQueueIndex,
                    .image = overlayDrawColorImage,
                    .subresourceRange = vk::wholeColorSubresourceRange,
                },
            });

    overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkImageBlit imageBlit{};
    imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlit.srcSubresource.mipLevel = 0;
    imageBlit.srcSubresource.baseArrayLayer = 0;
    imageBlit.srcSubresource.layerCount = 1;
    imageBlit.srcOffsets[0] = {0, 0, 0};
    imageBlit.srcOffsets[1] = {static_cast<int>(overlayPostColorImage->width()),
                               static_cast<int>(overlayPostColorImage->height()), 1};
    imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlit.dstSubresource.mipLevel = 0;
    imageBlit.dstSubresource.baseArrayLayer = 0;
    imageBlit.dstSubresource.layerCount = 1;
    imageBlit.dstOffsets[0] = {0, 0, 0};
    imageBlit.dstOffsets[1] = {static_cast<int>(overlayDrawColorImage->width()),
                               static_cast<int>(overlayDrawColorImage->height()), 1};

    vkCmdBlitImage(context->overlayCommandBuffer->vkCommandBuffer(), overlayPostColorImage->vkImage(),
                   overlayPostColorImage->imageLayout(), overlayDrawColorImage->vkImage(),
                   overlayDrawColorImage->imageLayout(), 1, &imageBlit, VK_FILTER_LINEAR);

    context->overlayCommandBuffer->barriersBufferImage(
        {}, {{
                 .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 .oldLayout = overlayPostColorImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = overlayPostColorImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .oldLayout = overlayDrawColorImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = overlayDrawColorImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             }});
        overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

}

void UIModuleContext::postBlur(int times) {
    int postID = Renderer::instance().buffers()->getPostID();
    for (int i = postID - times + 1; i <= postID; i++) {
        postOverlay(BLUR, Renderer::instance().buffers()->overlayPostUniformOffset(i));
    }
}

void UIModuleContext::postEntityEffect(OverlayPostPipelineType type) {
    if (type != CREEPER && type != SPIDER && type != INVERT) {
        throw std::invalid_argument("Unsupported vanilla entity post-effect type");
    }
    if (type == SPIDER) {
        postSpider();
    } else {
        postOverlay(type, 0);
    }
    // This entry is called at GameRenderer's world-post boundary, before HUD/UI.
    // Preserve the same postprocessing on the scene tagged to Streamline.
    auto context = frameworkContext.lock();
    context->framework.lock()->captureFrameGenerationHudless(*context, overlayDrawColorImage);

}

void UIModuleContext::postSpider() {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto module = uiModule.lock();
    if (!framework->isRunning()) return;

    if (overlayMode == DRAW || overlayMode == POST || overlayMode == DIAGRAM_DRAW) {
        context->overlayCommandBuffer->endRenderPass();
        if (overlayMode == DRAW) {
            overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            overlayDrawDepthStencilImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        } else if (overlayMode == POST) {
            overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        } else {
            diagramDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            diagramDrawDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
    }
    overlayMode = NONE;

    auto mainQueueIndex = context->physicalDevice->mainQueueIndex();
    auto transition = [&](const std::shared_ptr<vk::DeviceLocalImage> &image,
                          VkImageLayout newLayout) {
        if (image->imageLayout() == newLayout) return;
        context->overlayCommandBuffer->barriersBufferImage(
            {}, {{
                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                .oldLayout = image->imageLayout(),
                .newLayout = newLayout,
                .srcQueueFamilyIndex = mainQueueIndex,
                .dstQueueFamilyIndex = mainQueueIndex,
                .image = image,
                .subresourceRange = vk::wholeColorSubresourceRange,
            }});
        image->imageLayout() = newLayout;
    };

    transition(overlayDrawColorImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    auto drawPass = [&](OverlayPostPipelineType type,
                        const std::shared_ptr<vk::DeviceLocalImage> &target,
                        const std::shared_ptr<vk::Framebuffer> &framebuffer) {
        transition(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        context->overlayCommandBuffer->beginRenderPass({
            .renderPass = module->overlayPostRenderPass_,
            .framebuffer = framebuffer,
            .renderAreaExtent = {target->width(), target->height()},
            .clearValues = {{.color = {0.0f, 0.0f, 0.0f, 0.0f}}},
        });
        vkCmdBindPipeline(context->overlayCommandBuffer->vkCommandBuffer(),
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          module->overlayPostPipelines_.at(type)->vkPipeline());
        uint32_t dynamicOffsets[] = {0, 0};
        vkCmdBindDescriptorSets(context->overlayCommandBuffer->vkCommandBuffer(),
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                overlayDescriptorTable->vkPipelineLayout(), 0,
                                overlayDescriptorTable->descriptorSet().size(),
                                overlayDescriptorTable->descriptorSet().data(), 2,
                                dynamicOffsets);
        context->overlayCommandBuffer->draw(3, 1);
        context->overlayCommandBuffer->endRenderPass();
        target->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        transition(target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };

    // Exact spider.json target graph: main -> temp -> largeBlur, then
    // main -> temp -> smallBlur, followed by the five ordered eye composites.
    drawPass(SPIDER_BLUR_MAIN_H15, overlayPostColorImage, overlayPostFramebuffer);
    drawPass(SPIDER_BLUR_TEMP_V15, diagramDrawColorImage, spiderLargeBlurFramebuffer);
    drawPass(SPIDER_BLUR_MAIN_H7, overlayPostColorImage, overlayPostFramebuffer);
    drawPass(SPIDER_BLUR_TEMP_V7, spiderSmallBlurImage, spiderSmallBlurFramebuffer);

    transition(overlayPostColorImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    context->overlayCommandBuffer->beginRenderPass({
        .renderPass = module->overlayPostRenderPass_,
        .framebuffer = overlayPostFramebuffer,
        .renderAreaExtent = {overlayPostColorImage->width(), overlayPostColorImage->height()},
        .clearValues = {{.color = {0.0f, 0.0f, 0.0f, 0.0f}}},
    });
    vkCmdBindPipeline(context->overlayCommandBuffer->vkCommandBuffer(),
                      VK_PIPELINE_BIND_POINT_GRAPHICS,
                      module->overlayPostPipelines_.at(SPIDER)->vkPipeline());
    uint32_t dynamicOffsets[] = {0, 0};
    vkCmdBindDescriptorSets(context->overlayCommandBuffer->vkCommandBuffer(),
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            overlayDescriptorTable->vkPipelineLayout(), 0,
                            overlayDescriptorTable->descriptorSet().size(),
                            overlayDescriptorTable->descriptorSet().data(), 2,
                            dynamicOffsets);
    context->overlayCommandBuffer->draw(3, 1);
    context->overlayCommandBuffer->endRenderPass();
    overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    transition(overlayPostColorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(overlayDrawColorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit imageBlit{};
    imageBlit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    imageBlit.srcOffsets[1] = {static_cast<int32_t>(overlayPostColorImage->width()),
                               static_cast<int32_t>(overlayPostColorImage->height()), 1};
    imageBlit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    imageBlit.dstOffsets[1] = {static_cast<int32_t>(overlayDrawColorImage->width()),
                               static_cast<int32_t>(overlayDrawColorImage->height()), 1};
    vkCmdBlitImage(context->overlayCommandBuffer->vkCommandBuffer(),
                   overlayPostColorImage->vkImage(), overlayPostColorImage->imageLayout(),
                   overlayDrawColorImage->vkImage(), overlayDrawColorImage->imageLayout(),
                   1, &imageBlit, VK_FILTER_LINEAR);
    transition(overlayPostColorImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(overlayDrawColorImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void UIModuleContext::postDiagram() {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto module = uiModule.lock();

    if (!framework->isRunning() || overlayMode != DIAGRAM_DRAW) return;

    context->overlayCommandBuffer->endRenderPass();
    diagramDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    diagramDrawDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    overlayMode = NONE;

    auto mainQueueIndex = context->physicalDevice->mainQueueIndex();
    context->overlayCommandBuffer->barriersBufferImage(
        {}, {{
                 .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .oldLayout = diagramDrawColorImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = diagramDrawColorImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .oldLayout = diagramDrawDepthImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = diagramDrawDepthImage,
                 .subresourceRange = vk::wholeDepthSubresourceRange,
             }});
    diagramDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    diagramDrawDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    switchOverlayPost();
    int postID = Renderer::instance().buffers()->getPostID();
    if (postID < 0) {
        throw std::runtime_error("Diagram post-process uniform was not uploaded");
    }

    vkCmdBindPipeline(context->overlayCommandBuffer->vkCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                      module->overlayPostPipelines_[DIAGRAM]->vkPipeline());
    uint32_t dynamicOffsets[] = {0, Renderer::instance().buffers()->overlayPostUniformOffset(postID)};
    vkCmdBindDescriptorSets(context->overlayCommandBuffer->vkCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            overlayDescriptorTable->vkPipelineLayout(), 0,
                            overlayDescriptorTable->descriptorSet().size(),
                            overlayDescriptorTable->descriptorSet().data(), 2, dynamicOffsets);
    context->overlayCommandBuffer->draw(3, 1);

    context->overlayCommandBuffer->endRenderPass();
    overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    overlayMode = NONE;

    context->overlayCommandBuffer->barriersBufferImage(
        {}, {{
                 .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                 .oldLayout = overlayPostColorImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = overlayPostColorImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 .oldLayout = overlayDrawColorImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = overlayDrawColorImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             }});
    overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkImageBlit imageBlit{};
    imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlit.srcSubresource.mipLevel = 0;
    imageBlit.srcSubresource.baseArrayLayer = 0;
    imageBlit.srcSubresource.layerCount = 1;
    imageBlit.srcOffsets[0] = {0, 0, 0};
    imageBlit.srcOffsets[1] = {static_cast<int>(overlayPostColorImage->width()),
                               static_cast<int>(overlayPostColorImage->height()), 1};
    imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlit.dstSubresource.mipLevel = 0;
    imageBlit.dstSubresource.baseArrayLayer = 0;
    imageBlit.dstSubresource.layerCount = 1;
    imageBlit.dstOffsets[0] = {0, 0, 0};
    imageBlit.dstOffsets[1] = {static_cast<int>(overlayDrawColorImage->width()),
                               static_cast<int>(overlayDrawColorImage->height()), 1};
    vkCmdBlitImage(context->overlayCommandBuffer->vkCommandBuffer(), overlayPostColorImage->vkImage(),
                   overlayPostColorImage->imageLayout(), overlayDrawColorImage->vkImage(),
                   overlayDrawColorImage->imageLayout(), 1, &imageBlit, VK_FILTER_NEAREST);

    context->overlayCommandBuffer->barriersBufferImage(
        {}, {{
                 .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .oldLayout = overlayPostColorImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = overlayPostColorImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             },
             {
                 .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 .oldLayout = overlayDrawColorImage->imageLayout(),
                 .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 .srcQueueFamilyIndex = mainQueueIndex,
                 .dstQueueFamilyIndex = mainQueueIndex,
                 .image = overlayDrawColorImage,
                 .subresourceRange = vk::wholeColorSubresourceRange,
             }});
    overlayPostColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (diagramStateSaved) {
        overlayScissorEnabled = savedOverlayScissorEnabled;
        overlayScissor = savedOverlayScissor;
        overlayScissorGl = savedOverlayScissorGl;
        overlayViewport = savedOverlayViewport;
        overlayViewportGl = savedOverlayViewportGl;
        overlayDepthTestEnable = savedOverlayDepthTestEnable;
        overlayDepthWriteEnable = savedOverlayDepthWriteEnable;
        overlayDepthCompareOp = savedOverlayDepthCompareOp;
        diagramStateSaved = false;
    }
}

void UIModuleContext::refreshOverlayDescriptorTable() {
    auto context = frameworkContext.lock();
    auto module = uiModule.lock();
    module->refreshOverlayDescriptorTable(context->frameIndex);
}

void UIModuleContext::begin(std::shared_ptr<UIModuleContext> lastContext) {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;

    overlayMode = NONE;
    auto module = uiModule.lock();
    if (module == nullptr) return;
    module->prepareOverlayDescriptorTable(context->frameIndex);
    overlayDescriptorTable = module->overlayDescriptorTables_.at(context->frameIndex);
    context->overlayCommandBuffer->bindDescriptorTable(overlayDescriptorTable, VK_PIPELINE_BIND_POINT_GRAPHICS);

    if (lastContext != nullptr)
        syncFromContext(lastContext);
    else
        syncToCommandBuffer();

    module->lastActiveContext_ = shared_from_this();
}

void UIModuleContext::end() {
    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();

    if (!framework->isRunning()) return;
    if (overlayMode == FRAMEBUFFER_DRAW) {
        endFramebufferDraw();
        return;
    }

    if (overlayMode == DRAW) {
        context->overlayCommandBuffer->endRenderPass();
#ifdef USE_AMD
        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
#else
        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
#endif
        overlayDrawDepthStencilImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        captureMainAliases();
    } else if (overlayMode == DIAGRAM_DRAW) {
        context->overlayCommandBuffer->endRenderPass();
        diagramDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        diagramDrawDepthImage->imageLayout() = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        diagramStateSaved = false;
    } else if (overlayMode == POST) {
        context->overlayCommandBuffer->endRenderPass();

        auto mainQueueIndex = context->physicalDevice->mainQueueIndex();
        context->overlayCommandBuffer->barriersBufferImage(
            {}, {{
                    .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .oldLayout = overlayDrawColorImage->imageLayout(),
#ifdef USE_AMD
                    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
#else
                    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
#endif
                    .srcQueueFamilyIndex = mainQueueIndex,
                    .dstQueueFamilyIndex = mainQueueIndex,
                    .image = overlayDrawColorImage,
                    .subresourceRange = vk::wholeColorSubresourceRange,
                }});
#ifdef USE_AMD
        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
#else
        overlayDrawColorImage->imageLayout() = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
#endif
    }

    overlayMode = NONE;
}
