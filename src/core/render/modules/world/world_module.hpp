#pragma once

#include "common/shared.hpp"
#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/vulkan/all_core_vulkan.hpp"

#include <cstdint>
#include <map>

class Framework;
class FrameworkContext;
class WorldPipeline;
struct WorldPipelineContext;

struct WorldModuleContext;

struct MotionUpscalePushConstants {
    float scaleX;
    float scaleY;
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t dstWidth;
    uint32_t dstHeight;
};

// Small, size-independent state that remains meaningful across a GPU-idle rebuild.
// Temporal image histories must explicitly validate their own dimensions before opting in.
struct WorldModuleRebuildState {
    virtual ~WorldModuleRebuildState() = default;
};

class WorldModule {
  public:
    WorldModule();

    void init(std::shared_ptr<Framework> framework, std::shared_ptr<WorldPipeline> worldPipeline);

    virtual bool setOrCreateInputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                        std::vector<VkFormat> &formats,
                                        uint32_t frameIndex) = 0;
    virtual bool setOrCreateOutputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                         std::vector<VkFormat> &formats,
                                         uint32_t frameIndex) = 0;

    virtual void setAttributes(int attributeCount, std::vector<std::string> &attributeKVs) = 0;

    virtual void build() = 0;
    virtual std::vector<std::shared_ptr<WorldModuleContext>> &contexts() = 0;

    virtual void
    bindTexture(std::shared_ptr<vk::Sampler> sampler, std::shared_ptr<vk::DeviceLocalImage> image, int index) = 0;

    // Called at a GPU-idle resource-generation boundary. Modules with temporal state should
    // discard history before consuming the newly published material set.
    virtual void onResourceReload() {}

    virtual std::shared_ptr<WorldModuleRebuildState> captureRebuildState() const { return {}; }
    virtual void restoreRebuildState(const std::shared_ptr<WorldModuleRebuildState> &) {}

    // release resources that must be released before deconstruction
    virtual void preClose() = 0;

  protected:
    std::weak_ptr<Framework> framework_;
    std::weak_ptr<WorldPipeline> worldPipeline_;
};

struct WorldModuleContext {
    std::weak_ptr<FrameworkContext> frameworkContext;
    std::weak_ptr<WorldPipelineContext> worldPipelineContext;

    WorldModuleContext(std::shared_ptr<FrameworkContext> frameworkContext,
                       std::shared_ptr<WorldPipelineContext> worldPipelineContext);

    virtual void render() = 0;
};
