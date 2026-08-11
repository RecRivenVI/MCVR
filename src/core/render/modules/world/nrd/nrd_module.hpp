#pragma once

#include "core/render/modules/world/nrd/nrd_wrapper.hpp"
#include "core/render/modules/world/world_module.hpp"
#include "core/render/renderer.hpp"
#include <array>
#include <map>
#include <memory>
#include <vector>

struct NrdModuleContext;

class NrdModule : public WorldModule, public SharedObject<NrdModule> {
    friend class NrdModuleContext;

  public:
    static constexpr auto NAME = "render_pipeline.module.nrd.name";
    static constexpr uint32_t inputImageNum = 14;
    static constexpr uint32_t outputImageNum = 1;

    NrdModule();

    ~NrdModule();

    void init(std::shared_ptr<Framework> framework, std::shared_ptr<WorldPipeline> worldPipeline);

    bool setOrCreateInputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                std::vector<VkFormat> &formats,
                                uint32_t frameIndex) override;

    bool setOrCreateOutputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                 std::vector<VkFormat> &formats,
                                 uint32_t frameIndex) override;

    void build() override;

    void setAttributes(int attributeCount, std::vector<std::string> &attributeKVs) override;

    std::vector<std::shared_ptr<WorldModuleContext>> &contexts() override;

    void
    bindTexture(std::shared_ptr<vk::Sampler> sampler, std::shared_ptr<vk::DeviceLocalImage> image, int index) override;

    void onResourceReload() override;

    void preClose() override;

  private:
    static nrd::ReblurSettings makeDefaultReblurSettings();
    void updateReblurSettings();

    void initDescriptorTables();
    void initImages();
    void initPipeline();

  private:
    // NRD Settings
    nrd::ReblurSettings reblurSettings_ = {};

    // Input
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> diffuseIndirectRadianceImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> specularIndirectRadianceImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> directRadianceImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> diffuseAlbedoMetallicImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> specularAlbedoImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> normalRoughnessImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> motionVectorImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> linearDepthImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> clearRadianceImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> baseEmissionImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> fogImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> diffuseHitDepthImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> specularHitDepthImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> refractionRadianceImages_;

    // NRD
    // prepare
    std::shared_ptr<vk::ComputePipeline> preparePipeline_;
    std::vector<std::shared_ptr<vk::DescriptorTable>> prepareDescriptorTables_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> nrdDiffuseRadianceImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> nrdSpecularRadianceImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> nrdMotionVectorImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> nrdNormalRoughnessImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> nrdLinearDepthImages_;

    // denoise
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> denoisedDiffuseRadianceImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> denoisedSpecularRadianceImages_;

    // composition
    std::shared_ptr<vk::ComputePipeline> composePipeline_;
    std::vector<std::shared_ptr<vk::DescriptorTable>> composeDescriptorTables_;
    std::array<std::shared_ptr<vk::Sampler>, 2> composeSamplers_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> refractionHistoryRadianceImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> refractionHistoryDepthImages_;



    // output
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> denoisedRadianceImages_;

    struct ViewHistory {
        std::shared_ptr<NrdWrapper> wrapper_;
        int32_t lastRefractionHistoryFrameIndex_ = -1;
        uint32_t nrdFrameIndex_ = 0;
    };
    std::vector<ViewHistory> histories_;
    uint32_t width_, height_;


    std::vector<std::shared_ptr<std::array<std::shared_ptr<vk::DeviceLocalImage>, size_t(nrd::ResourceType::MAX_NUM)>>>
        userTexturePools_;

    std::vector<std::shared_ptr<WorldModuleContext>> contexts_;
};

struct NrdModuleContext : public WorldModuleContext, public SharedObject<NrdModuleContext> {
  public:
    NrdModuleContext(std::shared_ptr<FrameworkContext> frameworkContext,
                     std::shared_ptr<WorldPipelineContext> worldPipelineContext,
                     std::shared_ptr<NrdModule> nrdModule);

    void render() override;

  private:
    std::weak_ptr<NrdModule> nrdModule;

    // Input
    std::shared_ptr<vk::DeviceLocalImage> diffuseIndirectRadianceImage;
    std::shared_ptr<vk::DeviceLocalImage> specularIndirectRadianceImage;
    std::shared_ptr<vk::DeviceLocalImage> directRadianceImage;
    std::shared_ptr<vk::DeviceLocalImage> diffuseAlbedoMetallicImage;
    std::shared_ptr<vk::DeviceLocalImage> specularAlbedoImage;
    std::shared_ptr<vk::DeviceLocalImage> normalRoughnessImage;
    std::shared_ptr<vk::DeviceLocalImage> motionVectorImage;
    std::shared_ptr<vk::DeviceLocalImage> linearDepthImage;
    std::shared_ptr<vk::DeviceLocalImage> clearRadianceImage;
    std::shared_ptr<vk::DeviceLocalImage> baseEmissionImage;
    std::shared_ptr<vk::DeviceLocalImage> fogImage;
    std::shared_ptr<vk::DeviceLocalImage> diffuseHitDepthImage;
    std::shared_ptr<vk::DeviceLocalImage> specularHitDepthImage;
    std::shared_ptr<vk::DeviceLocalImage> refractionRadianceImage;

    // NRD
    std::shared_ptr<vk::DeviceLocalImage> nrdDiffuseRadianceImage;
    std::shared_ptr<vk::DeviceLocalImage> nrdSpecularRadianceImage;
    std::shared_ptr<vk::DeviceLocalImage> nrdMotionVectorImage;
    std::shared_ptr<vk::DeviceLocalImage> nrdNormalRoughnessImage;
    std::shared_ptr<vk::DeviceLocalImage> nrdLinearDepthImage;

    std::shared_ptr<vk::DeviceLocalImage> denoisedDiffuseRadianceImage;
    std::shared_ptr<vk::DeviceLocalImage> denoisedSpecularRadianceImage;
    std::shared_ptr<vk::DeviceLocalImage> refractionHistoryRadianceImage;
    std::shared_ptr<vk::DeviceLocalImage> refractionHistoryDepthImage;

    std::shared_ptr<vk::DescriptorTable> prepareDescriptorTable;
    std::shared_ptr<vk::DescriptorTable> composeDescriptorTable;

    std::shared_ptr<std::array<std::shared_ptr<vk::DeviceLocalImage>, size_t(nrd::ResourceType::MAX_NUM)>>
        userTexturePool;

    // output
    std::shared_ptr<vk::DeviceLocalImage> denoisedRadianceImage;
};
