#pragma once

#include "common/shared.hpp"
#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/vulkan/all_core_vulkan.hpp"
#include "core/render/framebuffers.hpp"

#include <array>
#include <map>
#include <mutex>
#include <unordered_map>
#include <optional>

class Framework;
class FrameworkContext;
struct WorldPipelineContext;

struct GraphicsPipelineShaderInfo {
    std::string vertexShaderFile;
    std::string fragmentShaderFile;
    VkPrimitiveTopology topology;
};

struct GraphicsPipelineShaders {
    std::shared_ptr<vk::Shader> vertexShader;
    std::shared_ptr<vk::Shader> tessellationControlShader;
    std::shared_ptr<vk::Shader> tessellationEvaluationShader;
    std::shared_ptr<vk::Shader> fragmentShader;
};

struct OverlayDynamicDrawShaderInfo {
    std::string key;
    uint32_t vertexFormatType;
    uint32_t drawMode;
    uint32_t uniformSize;
    std::string vertexShaderPath;
    std::string tessellationControlShaderPath;
    std::string tessellationEvaluationShaderPath;
    std::string fragmentShaderPath;
    uint32_t patchControlPoints = 0;
    std::unordered_map<std::string, std::string> definitions;
    VkPrimitiveTopology topology;
    GraphicsPipelineShaders shaders;
    vk::VertexLayoutInfo vertexLayout;
    bool customVertexLayout = false;
    std::shared_ptr<vk::DynamicGraphicsPipeline> pipeline;
};

struct OverlayTextureBinding {
    enum class FrameAlias : uint8_t { None, MainColor, MainDepth };

    std::shared_ptr<vk::Sampler> sampler;
    std::shared_ptr<vk::DeviceLocalImage> image;
    FrameAlias frameAlias = FrameAlias::None;
};

struct CustomVertexBufferBinding {
    uint32_t binding;
    VkDeviceSize offset;
    std::shared_ptr<vk::DeviceLocalBuffer> buffer;
};

enum OverlayPostPipelineType {
    BLUR,
    DIAGRAM,
    CREEPER,
    SPIDER,
    INVERT,
    SPIDER_BLUR_MAIN_H15,
    SPIDER_BLUR_TEMP_V15,
    SPIDER_BLUR_MAIN_H7,
    SPIDER_BLUR_TEMP_V7,
    MAX_OVERLAY_POST_PIPELINE_TYPE,
};

enum OverlayMode {
    NONE,
    DRAW,
    DIAGRAM_DRAW,
    FRAMEBUFFER_DRAW,
    POST,
};

class UIModuleContext;

class UIModule : public SharedObject<UIModule> {
    friend UIModuleContext;

  public:
    UIModule();
    ~UIModule();
    // Generic UI PT service: immutable compute state is shared; view histories and
    // in-flight outputs remain per view while Ponder transition geometry shares one world.
    std::shared_ptr<vk::ComputePipeline> ponderPathPipeline;
    std::shared_ptr<class PonderSharedWorld> ponderSharedWorld;
    std::map<uint64_t, std::shared_ptr<class PonderSceneRenderer>> ponderScenes;
    std::vector<std::weak_ptr<class PonderSharedWorld>> ponderSceneLifetimes;
    uint64_t uiPtViewCreates = 0;
    uint64_t uiPtGeometryBuilds = 0;
    uint64_t uiPtCompositeCreates = 0;
    uint64_t uiPtPipelineCreates = 0;
    uint64_t uiPtMaterialUpdates = 0;
    bool uiPtCollecting = false;
    // Call only at the pipeline's GPU-idle rebuild/reload/shutdown boundary.
    void closePonderScenes();

    void init(std::shared_ptr<Framework> framework);
    void resize(std::shared_ptr<Framework> framework);
    std::vector<std::shared_ptr<UIModuleContext>> &contexts();
    std::vector<std::shared_ptr<vk::DescriptorTable>> &overlayDescriptorTables();
    const std::vector<OverlayDynamicDrawShaderInfo> &overlayDynamicDrawShaders() const;
    uint32_t registerOverlayDrawShader(const std::string &key,
                                       uint32_t vertexFormatType,
                                       uint32_t drawMode,
                                       uint32_t uniformSize,
                                       const std::string &vertexShaderPath,
                                       const std::string &fragmentShaderPath,
                                       const std::string &tessellationControlShaderPath,
                                       const std::string &tessellationEvaluationShaderPath,
                                       uint32_t patchControlPoints,
                                       const std::optional<vk::VertexLayoutInfo> &customVertexLayout,
                                       const std::unordered_map<std::string, std::string> &definitions);
    const OverlayDynamicDrawShaderInfo &overlayDrawShaderInfo(uint32_t shaderId) const;

    // Temporary performance diagnostics: resource generation counters after rebuilds.
    std::string resourceDiagnostics() const;

    void bindTexture(std::shared_ptr<vk::Sampler> sampler, std::shared_ptr<vk::DeviceLocalImage> image, int index);
    void bindFrameAlias(int index, bool depth, std::shared_ptr<vk::Sampler> sampler);
    void unbindTexture(int index);
    void prepareOverlayDescriptorTable(uint32_t frameIndex);
    bool defaultStencilAvailable() const;
    void refreshOverlayDescriptorTable(uint32_t frameIndex);

  private:
    std::shared_ptr<vk::DescriptorTable> createOverlayDescriptorTable();
    void bindOverlayDescriptorTableResources(std::shared_ptr<vk::DescriptorTable> descriptorTable, uint32_t frameIndex);
    void initOverlayDescriptorTablesAndFrameSamplers();

    void initOverlayDrawImages();
    void initMainAliasImages();
    void initMainDepthPass();
    void initOverlayDrawRenderPass();
    void initOverlayDrawFrameBuffers();

    void initDiagramDrawImages();
    void initDiagramDrawRenderPass();
    void initDiagramDrawFrameBuffers();

    void initOverlayPostImages();
    void initSpiderBlurImages();
    void initOverlayPostRenderPass();
    void initOverlayPostFrameBuffers();
    void initSpiderBlurFrameBuffers();
    void initOverlayPostPipelineTypes();
    void initOverlayPostPipelines();

  private:
    std::weak_ptr<Framework> framework_;
    std::vector<std::shared_ptr<vk::DescriptorTable>> overlayDescriptorTables_;

    std::vector<std::shared_ptr<vk::DeviceLocalImage>> overlayDrawColorImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> overlayDrawDepthStencilImages_;
    std::vector<uint32_t> overlayDrawDepthStencilViewIndices_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> mainColorAliasImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> mainDepthAliasImages_;
    std::shared_ptr<vk::RenderPass> overlayDrawRenderPass_;
    std::vector<std::shared_ptr<vk::Framebuffer>> overlayDrawFramebuffers_;
    std::unordered_map<std::string, uint32_t> overlayDynamicDrawShaderIds_;
    std::vector<OverlayDynamicDrawShaderInfo> overlayDynamicDrawShaders_;
    std::unordered_map<int, OverlayTextureBinding> overlayTextureBindings_;
    std::vector<bool> overlayDescriptorTableDirty_;
    mutable std::recursive_mutex overlayDescriptorMutex_;
    bool defaultStencilAvailable_ = false;
    std::shared_ptr<vk::RenderPass> mainDepthRenderPass_;
    std::vector<std::shared_ptr<vk::Framebuffer>> mainDepthFramebuffers_;
    std::vector<std::shared_ptr<vk::DescriptorTable>> mainDepthDescriptorTables_;
    std::vector<std::shared_ptr<vk::Sampler>> mainDepthSourceSamplers_;
    std::shared_ptr<vk::DynamicGraphicsPipeline> mainDepthPipeline_;
    std::map<std::string, std::shared_ptr<vk::RenderPass>> framebufferRenderPasses_;
    std::map<std::pair<uint32_t, std::string>, std::shared_ptr<vk::DynamicGraphicsPipeline>> framebufferPipelines_;
    std::map<std::string, std::shared_ptr<vk::DynamicGraphicsPipeline>> framebufferClearPipelines_;
    std::map<std::string, std::shared_ptr<vk::DynamicGraphicsPipeline>> framebufferBlitPipelines_;

    std::vector<std::shared_ptr<vk::DeviceLocalImage>> diagramDrawColorImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> diagramDrawDepthImages_;
    std::vector<std::shared_ptr<vk::Sampler>> diagramDrawColorSamplers_;
    std::vector<std::shared_ptr<vk::Sampler>> diagramDrawDepthSamplers_;
    std::shared_ptr<vk::RenderPass> diagramDrawRenderPass_;
    std::vector<std::shared_ptr<vk::Framebuffer>> diagramDrawFramebuffers_;

    std::vector<std::shared_ptr<vk::DeviceLocalImage>> overlayPostColorImages_;
    std::vector<std::shared_ptr<vk::Sampler>> overlayDrawColorImageSamplers_;
    std::vector<std::shared_ptr<vk::Sampler>> overlayPostColorImageSamplers_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> spiderSmallBlurImages_;
    std::vector<std::shared_ptr<vk::Sampler>> spiderSmallBlurSamplers_;
    std::shared_ptr<vk::RenderPass> overlayPostRenderPass_;
    std::vector<std::shared_ptr<vk::Framebuffer>> overlayPostFramebuffers_;
    std::vector<std::shared_ptr<vk::Framebuffer>> spiderLargeBlurFramebuffers_;
    std::vector<std::shared_ptr<vk::Framebuffer>> spiderSmallBlurFramebuffers_;
    std::map<OverlayPostPipelineType, GraphicsPipelineShaderInfo> overlayPostPipelineInfos_;
    std::map<OverlayPostPipelineType, GraphicsPipelineShaders> overlayPostPipelineShaders_;
    std::map<OverlayPostPipelineType, std::shared_ptr<vk::GraphicsPipeline>> overlayPostPipelines_;

    std::vector<std::shared_ptr<UIModuleContext>> contexts_;
    std::weak_ptr<UIModuleContext> lastActiveContext_;
};

struct UIModuleContext : public SharedObject<UIModuleContext> {
    std::weak_ptr<FrameworkContext> frameworkContext;
    std::weak_ptr<UIModule> uiModule;

    bool overlayScissorEnabled;
    VkRect2D overlayScissor;
    std::array<int, 4> overlayScissorGl{};
    VkViewport overlayViewport;
    std::array<int, 4> overlayViewportGl{};

    VkBool32 overlayBlendEnabled;
    VkColorBlendEquationEXT overlayColorBlendEquation;
    VkColorComponentFlags overlayColorWriteMask;
    bool overlayColorLogicOpEnable;
    VkLogicOp overlayColorLogicOp;
    std::array<float, 4> overlayBlendConstants;

    bool overlayDepthTestEnable;
    bool overlayDepthWriteEnable;
    VkCompareOp overlayDepthCompareOp;
    bool overlayStencilTestEnable;
    std::array<VkStencilOp, 2> overlayFailOp; // for front and back face
    std::array<VkStencilOp, 2> overlayPassOp;
    std::array<VkStencilOp, 2> overlayDepthFailOp;
    std::array<VkCompareOp, 2> overlayCompareOp;
    std::array<uint32_t, 2> overlayReference;
    std::array<uint32_t, 2> overlayCompareMask;
    std::array<uint32_t, 2> overlayWriteMask;

    VkCullModeFlags overlayCullMode;
    VkFrontFace overlayFrontFace;
    VkPolygonMode overlayPolygonMode;
    bool overlayDepthBiasEnable;
    std::array<float, 3> overlayDepthBiasConstantFactor; // for 3 types of polygon mode
    std::array<float, 3> overlayDepthBiasClamp;
    std::array<float, 3> overlayDepthBiasSlopeFactor;
    float overlayLineWidth;

    std::array<float, 4> overlayClearColors;
    float overlayClearDepth;
    uint32_t overlayClearStencil;

    OverlayMode overlayMode = NONE;
    Framebuffers::Snapshot activeFramebuffer;
    std::shared_ptr<vk::Framebuffer> activeFramebufferHandle;
    std::string activeFramebufferPassKey;

    std::shared_ptr<vk::DescriptorTable> overlayDescriptorTable;
    std::shared_ptr<vk::DeviceLocalImage> overlayDrawColorImage;
    std::shared_ptr<vk::DeviceLocalImage> overlayDrawDepthStencilImage;
    uint32_t overlayDrawDepthStencilViewIndex = 0;
    std::shared_ptr<vk::DeviceLocalImage> mainColorAliasImage;
    std::shared_ptr<vk::DeviceLocalImage> mainDepthAliasImage;
    std::shared_ptr<vk::Framebuffer> mainDepthFramebuffer;
    std::shared_ptr<vk::DescriptorTable> mainDepthDescriptorTable;
    std::shared_ptr<vk::Framebuffer> overlayDrawFramebuffer;
    std::shared_ptr<vk::Framebuffer> hudlessModulationFramebuffer;
    std::shared_ptr<vk::DeviceLocalImage> hudlessModulationImage;
    std::shared_ptr<vk::DeviceLocalImage> hudlessModulationDepth;
    uint32_t hudlessModulationDepthView = 0;
    bool mirroringBackgroundModulation = false;
    bool backgroundModulationPrepared = false;
    void prepareBackgroundModulation();
    void mirrorBackgroundModulation(const std::function<void()> &draw);
    std::shared_ptr<vk::DeviceLocalImage> overlayPostColorImage;
    std::shared_ptr<vk::DeviceLocalImage> spiderSmallBlurImage;
    std::shared_ptr<vk::Sampler> overlayDrawColorImageSampler;
    std::shared_ptr<vk::Framebuffer> overlayPostFramebuffer;
    std::shared_ptr<vk::Framebuffer> spiderLargeBlurFramebuffer;
    std::shared_ptr<vk::Framebuffer> spiderSmallBlurFramebuffer;
    std::shared_ptr<vk::DeviceLocalImage> diagramDrawColorImage;
    std::shared_ptr<vk::DeviceLocalImage> diagramDrawDepthImage;
    std::shared_ptr<vk::Framebuffer> diagramDrawFramebuffer;

    bool diagramStateSaved;
    struct DiagramTargetState {
        std::shared_ptr<UIModuleContext> previousState;
        Framebuffers::Snapshot source;
        uint32_t readFramebuffer;
        uint32_t drawFramebuffer;
    };
    std::vector<DiagramTargetState> diagramTargetStack;
    bool savedOverlayScissorEnabled;
    VkRect2D savedOverlayScissor;
    std::array<int, 4> savedOverlayScissorGl{};
    VkViewport savedOverlayViewport;
    std::array<int, 4> savedOverlayViewportGl{};
    bool savedOverlayDepthTestEnable;
    bool savedOverlayDepthWriteEnable;
    VkCompareOp savedOverlayDepthCompareOp;

    UIModuleContext(std::shared_ptr<FrameworkContext> context, std::shared_ptr<UIModule> uiModule);

    bool deferIdleStateWrite() const;
    void syncToCommandBuffer();
    void writeMainDepth(std::shared_ptr<WorldPipelineContext> worldContext);
    void captureMainAliases();
    Framebuffers::Snapshot framebufferSnapshot(uint32_t target) const;
    bool switchFramebufferDraw();
    void endFramebufferDraw();
    VkExtent2D drawExtent() const;
    uint32_t drawAttachmentCount() const;
    void syncColorAttachments();
    void clearMaskedFramebuffer(bool color, bool depth, bool stencil);
    std::shared_ptr<vk::DynamicGraphicsPipeline> framebufferPipeline(uint32_t shaderId);
    void blitFramebuffer(int sx0, int sy0, int sx1, int sy1,
                         int dx0, int dy0, int dx1, int dy1, int mask, int filter);
    void blitAttachment(const Framebuffers::ResolvedAttachment &source,
                         const Framebuffers::ResolvedAttachment &destination,
                         int sx0, int sy0, int sx1, int sy1,
                         int dx0, int dy0, int dx1, int dy1, bool depth, int filter);
    void syncFromContext(std::shared_ptr<UIModuleContext> other);
    void copyPersistentStateFrom(const UIModuleContext &other);

    void setOverlayScissorEnabled(bool enabled);
    void setOverlayScissor(int x, int y, int width, int height);
    void setOverlayViewport(int x, int y, int width, int height);

    void setOverlayBlendEnable(bool enable);
    void setOverlayColorBlendConstants(float const1, float const2, float const3, float const4);
    void setOverlayColorLogicOpEnable(bool enable);
    void setOverlayBlendFuncSeparate(int srcColorBlendFactor,
                                     int srcAlphaBlendFactor,
                                     int dstColorBlendFactor,
                                     int dstAlphaBlendFactor);
    void setOverlayBlendOpSeparate(int colorBlendOp, int alphaBlendOp);
    void setOverlayColorWriteMask(int colorWriteMask);
    void setOverlayColorLogicOp(int colorLogicOp);

    void setOverlayDepthTestEnable(bool enable);
    void setOverlayDepthWriteEnable(bool enable);
    void setOverlayStencilTestEnable(bool enable);
    void setOverlayDepthCompareOp(int depthCompareOp);
    void setOverlayStencilFrontFunc(int compareOp, int reference, int compareMask);
    void setOverlayStencilBackFunc(int compareOp, int reference, int compareMask);
    void setOverlayStencilFrontOp(int failOp, int depthFailOp, int passOp);
    void setOverlayStencilBackOp(int failOp, int depthFailOp, int passOp);
    void setOverlayStencilFrontWriteMask(int writeMask);
    void setOverlayStencilBackWriteMask(int writeMask);

    void setOverlayLineWidth(float lineWidth);
    void setOverlayPolygonMode(int polygonMode);
    void setOverlayCullMode(int cullMode);
    void setOverlayFrontFace(int frontFace);
    void setOverlayDepthBiasEnable(int polygonMode, bool enable);
    void setOverlayDepthBias(float depthBiasSlopeFactor, float depthBiasConstantFactor);

    void setOverlayClearColor(float red, float green, float blue, float alpha);
    void setOverlayClearDepth(double depth);
    void setOverlayClearStencil(int stencil);

    void switchOverlayDraw();
    void switchOverlayPost();
    void beginDiagram(int x, int y, int width, int height);
    void postDiagram();
    void beginDiagramTarget(uint32_t framebuffer, int width, int height);
    void postDiagramTarget(uint32_t framebuffer);
    void abortDiagramTarget();

    void clearOverlayEntireColorAttachment();
    void clearOverlayEntireDepthStencilAttachment(int aspectMask);

    void drawIndexed(std::shared_ptr<vk::DeviceLocalBuffer> vertexBuffer,
                     std::shared_ptr<vk::DeviceLocalBuffer> indexBuffer,
                     std::shared_ptr<vk::DeviceLocalBuffer> patchIndexBuffer,
                     uint32_t shaderId,
                     uint32_t uniformOffset,
                     uint32_t indexCount,
                     uint32_t patchIndexCount,
                     VkIndexType indexType);
    void drawCustomVertexArray(
        const std::vector<CustomVertexBufferBinding> &vertexBuffers,
        std::shared_ptr<vk::DeviceLocalBuffer> indexBuffer,
        std::shared_ptr<vk::DeviceLocalBuffer> indirectBuffer,
        uint32_t shaderId, uint32_t uniformOffset, uint32_t indexCount,
        uint32_t instanceCount, VkIndexType indexType, VkDeviceSize indirectOffset,
        uint32_t indirectDrawCount, uint32_t indirectStride);

    void postBlur(int times = 1);
    void postEntityEffect(OverlayPostPipelineType type);
    void postSpider();
    void postOverlay(OverlayPostPipelineType type, uint32_t uniformOffset);
    void refreshOverlayDescriptorTable();

    void begin(std::shared_ptr<UIModuleContext> lastContext);
    void end();
};
