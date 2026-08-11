#pragma once

#include "common/shared.hpp"
#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/vulkan/all_core_vulkan.hpp"
#include <chrono>
#include <cstdint>

#include "core/render/modules/world/world_module.hpp"

class Framework;
class FrameworkContext;
class WorldPipeline;
struct WorldModuleContext;

struct ToneMappingModuleContext;

struct ToneMappingModuleExposureData {
    float exposure;
    float avgLogLum;
    uint32_t historyValid;
    uint32_t padding;
};
static_assert(sizeof(ToneMappingModuleExposureData) == 16);

enum ToneMappingMethod : int32_t {
    TONE_MAPPING_METHOD_PBR_NEUTRAL = 0,
    TONE_MAPPING_METHOD_REINHARD = 1,
    TONE_MAPPING_METHOD_REINHARD_WHITE_POINT = 2,
    TONE_MAPPING_METHOD_ACES_FITTED = 3,
    TONE_MAPPING_METHOD_ACES_FITTED_WHITE_POINT = 4,
    TONE_MAPPING_METHOD_UNCHARTED2 = 5,
};

enum ToneMappingExposureMeteringMode : int32_t {
    TONE_MAPPING_EXPOSURE_METERING_MODE_GLOBAL = 0,
    TONE_MAPPING_EXPOSURE_METERING_MODE_CENTER = 1,
};

struct ToneMappingModulePushConstant {
    float log2Min;
    float log2Max;
    float epsilon;
    float lowPercent;
    float highPercent;
    float middleGrey;
    float dt;
    float speedUp;
    float speedDown;
    float minExposure;
    float maxExposure;
    float manualExposure;
    float exposureBias;
    float whitePoint;
    float saturation;
    int toneMappingMethod;
    int autoExposure;
    int clampOutput;
    int exposureMeteringMode;
    float centerMeteringPercent;
    float padding0;
    float padding1;
    float padding2;
};

class ToneMappingModule : public WorldModule, public SharedObject<ToneMappingModule> {
    friend ToneMappingModuleContext;

  public:
    constexpr static std::string_view NAME = "render_pipeline.module.tone_mapping.name";
    constexpr static uint32_t inputImageNum = 1;
    constexpr static uint32_t outputImageNum = 1;

    ToneMappingModule();

    void init(std::shared_ptr<Framework> framework, std::shared_ptr<WorldPipeline> worldPipeline);

    bool setOrCreateInputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                std::vector<VkFormat> &formats,
                                uint32_t frameIndex) override;
    bool setOrCreateOutputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                 std::vector<VkFormat> &formats,
                                 uint32_t frameIndex) override;

    void setAttributes(int attributeCount, std::vector<std::string> &attributeKVs) override;

    void build() override;

    std::vector<std::shared_ptr<WorldModuleContext>> &contexts() override;

    void
    bindTexture(std::shared_ptr<vk::Sampler> sampler, std::shared_ptr<vk::DeviceLocalImage> image, int index) override;

    void preClose() override;
    std::shared_ptr<WorldModuleRebuildState> captureRebuildState() const override;
    void restoreRebuildState(const std::shared_ptr<WorldModuleRebuildState> &state) override;

  private:
    static constexpr uint32_t histSize = 256;

    void initDescriptorTables();
    void initImages();
    void initBuffers();
    void initRenderPass();
    void initFrameBuffers();
    void initPipeline();

  private:
    // input
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> hdrImages_;

    // tone mapping
    std::vector<std::shared_ptr<vk::DescriptorTable>> descriptorTables_;

    std::vector<std::shared_ptr<vk::DeviceLocalBuffer>> histBuffers_;
    struct ViewExposure {
        std::shared_ptr<vk::DeviceLocalBuffer> data;
        bool initialized = false;
        bool firstFrame = true;
    };
    std::vector<ViewExposure> exposures_{1};
    struct ExposureRebuildState : WorldModuleRebuildState {
        std::shared_ptr<vk::DeviceLocalBuffer> buffer;
        VkDevice device = VK_NULL_HANDLE;
        bool initialized = false;
    };

    std::shared_ptr<vk::Shader> histShader_;
    std::shared_ptr<vk::ComputePipeline> histPipeline_;

    std::shared_ptr<vk::Shader> exposureShader_;
    std::shared_ptr<vk::ComputePipeline> exposurePipeline_;

    std::shared_ptr<vk::Shader> vertShader_;
    std::shared_ptr<vk::Shader> fragShader_;
    std::shared_ptr<vk::RenderPass> renderPass_;
    std::vector<std::shared_ptr<vk::Framebuffer>> framebuffers_;
    std::shared_ptr<vk::GraphicsPipeline> pipeline_;
    std::vector<std::shared_ptr<vk::Sampler>> samplers_;

    float middleGrey_ = 0.18f;
    float speedUp_ = 3.0f;
    float speedDown_ = 3.0f;

    float log2Min_ = -12.0f;
    float log2Max_ = 4.0f;
    float epsilon_ = 1e-6f;
    float lowPercent_ = 0.005f;
    float highPercent_ = 0.99f;
    float minExposure_ = 1e-4f;
    float maxExposure_ = 1.2f;

    float manualExposure_ = 1.0f;
    float exposureBias_ = 0.0f;
    float whitePoint_ = 11.2f;
    float saturation_ = 1.0f;

    int toneMappingMethod_ = TONE_MAPPING_METHOD_ACES_FITTED;
    bool isAutoExposureEnabled_ = true;
    bool shouldClampOutput_ = true;
    int exposureMeteringMode_ = TONE_MAPPING_EXPOSURE_METERING_MODE_GLOBAL;
    float centerMeteringPercent_ = 20.0f;

    // output
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> ldrImages_;

    std::vector<std::shared_ptr<WorldModuleContext>> contexts_;

    std::chrono::time_point<std::chrono::high_resolution_clock> lastTimePoint_;

    uint32_t width_ = 0, height_ = 0;
};

struct ToneMappingModuleContext : public WorldModuleContext, SharedObject<ToneMappingModuleContext> {
    std::weak_ptr<ToneMappingModule> toneMappingModule;

    // input
    std::shared_ptr<vk::DeviceLocalImage> hdrImage;

    // tone mapping
    std::shared_ptr<vk::DescriptorTable> descriptorTable;
    std::shared_ptr<vk::Framebuffer> framebuffer;
    std::shared_ptr<vk::DeviceLocalBuffer> histBuffer;

    // output
    std::shared_ptr<vk::DeviceLocalImage> ldrImage;

    ToneMappingModuleContext(std::shared_ptr<FrameworkContext> frameworkContext,
                             std::shared_ptr<WorldPipelineContext> worldPipelineContext,
                             std::shared_ptr<ToneMappingModule> toneMappingModule);

    void render() override;
};
