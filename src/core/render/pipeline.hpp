#pragma once

#include "common/shared.hpp"
#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/vulkan/all_core_vulkan.hpp"

#include <atomic>
#include <functional>
#include <map>

struct WorldPipelineBuildParams {
    int moduleCount;
    int padding;
    char **moduleNames;
    int *imageFormats;
    int **inputIndices;
    int **outputIndices;
    int *attributeCounts;
    char*** attributeKVs;
};

class Framework;
class FrameworkContext;
class WorldModule;
class WorldModuleContext;
struct WorldModuleRebuildState;
class ShaderPack;
class UIModule;
class UIModuleContext;

class WorldPipeline;
struct WorldPipelineContext;

class Pipeline;
struct PipelineContext;

class WorldPipelineBlueprint : public SharedObject<WorldPipelineBlueprint> {
    friend WorldPipeline;

  public:
    WorldPipelineBlueprint(WorldPipelineBuildParams *params);

  private:
    std::vector<std::string> moduleNames_;
    std::vector<std::vector<uint32_t>> modulesInputIndices_;
    std::vector<std::vector<uint32_t>> modulesOutputIndices_;
    std::vector<VkFormat> imageFormats_;
    std::vector<uint32_t> attributeCounts_;
    std::vector<std::vector<std::string>> attributeKVs_;
};

class WorldPipeline : public SharedObject<WorldPipeline> {
    friend Pipeline;
    friend WorldPipelineContext;
    friend WorldPipelineBlueprint;

  public:
    WorldPipeline();

    void init(std::shared_ptr<Framework> framework, std::shared_ptr<Pipeline> pipeline,
              std::shared_ptr<ShaderPack> reusableShaderPack = nullptr,
              std::vector<std::shared_ptr<WorldModuleRebuildState>> rebuildStates = {},
              VkExtent2D renderExtent = {});

    std::vector<std::shared_ptr<WorldModule>> &worldModules();
    std::vector<std::shared_ptr<WorldPipelineContext>> &contexts();
    std::shared_ptr<ShaderPack> shaderPack();
    VkExtent2D renderExtent() const { return renderExtent_; }
    uint32_t viewCount() const { return viewCount_; }
    uint32_t framesPerView() const { return static_cast<uint32_t>(contexts_.size()) / viewCount_; }
    uint32_t viewForSlot(uint32_t slot) const { return slot / framesPerView(); }
    size_t uniqueDispatchImageCount() const;

    void bindTexture(std::shared_ptr<vk::Sampler> sampler, std::shared_ptr<vk::DeviceLocalImage> image, int index);
    void onResourceReload();

  private:
    void dumpSharedImages(const char *label) const;

    std::vector<std::string> auditModuleNames_;
    std::vector<std::shared_ptr<WorldModule>> worldModules_;
    std::vector<std::vector<std::shared_ptr<vk::DeviceLocalImage>>> sharedImages_;
    std::shared_ptr<ShaderPack> shaderPack_;
    bool emissionAtBuild_ = false;
    VkExtent2D renderExtent_{};
    uint32_t viewCount_ = 1;

    std::vector<std::shared_ptr<WorldPipelineContext>> contexts_;
};

struct WorldPipelineContext : public SharedObject<WorldPipelineContext> {
    std::weak_ptr<FrameworkContext> frameworkContext;
    std::weak_ptr<WorldPipeline> worldPipeline;

    std::shared_ptr<vk::DeviceLocalImage> outputImage;
    std::vector<std::shared_ptr<WorldModuleContext>> worldModuleContexts;

    WorldPipelineContext(std::shared_ptr<FrameworkContext> frameworkContext,
                         std::shared_ptr<WorldPipeline> worldPipeline);

    void render();
};

class Pipeline : public SharedObject<Pipeline> {
    friend PipelineContext;

  public:
    static std::map<
        std::string,
        std::function<std::shared_ptr<WorldModule>(std::shared_ptr<Framework>, std::shared_ptr<WorldPipeline>)>>
        worldModuleConstructors;
    static std::map<std::string, std::pair<uint32_t, uint32_t>> worldModuleInOutImageNums;
    static std::map<std::string, std::function<void()>> worldModuleStaticPreCloser;
    static void collectWorldModules();
    static void beginNativeRebuild();
    static void endNativeRebuild();
    static bool nativeRebuildActive();

  public:
    Pipeline();
    ~Pipeline();

    void init(std::shared_ptr<Framework> framework);
    void buildWorldPipelineBlueprint(WorldPipelineBuildParams *params);
    void recreate(std::shared_ptr<Framework> framework, bool resizeTargets, bool rebuildWorld);
    void close();
    std::shared_ptr<PipelineContext> acquirePipelineContext(std::shared_ptr<FrameworkContext> context);
    std::vector<std::shared_ptr<PipelineContext>> &contexts();
    void bindTexture(std::shared_ptr<vk::Sampler> sampler, std::shared_ptr<vk::DeviceLocalImage> image, int index);
    void onResourceReload();

    std::shared_ptr<UIModule> uiModule();
    std::shared_ptr<WorldPipeline> worldPipeline();

    std::shared_ptr<WorldPipelineBlueprint> worldPipelineBlueprint();

    bool isRecreationNeeded = false;

  private:
    static std::atomic<bool> nativeRebuildActive_;

    std::weak_ptr<Framework> framework_;

    std::shared_ptr<UIModule> uiModule_;
    std::shared_ptr<WorldPipeline> worldPipeline_;

    std::shared_ptr<WorldPipelineBlueprint> worldPipelineBlueprint_;

    std::shared_ptr<std::vector<std::shared_ptr<PipelineContext>>> contexts_;
};

struct PipelineContext : public SharedObject<PipelineContext> {
    std::weak_ptr<FrameworkContext> frameworkContext;

    std::shared_ptr<UIModuleContext> uiModuleContext;
    std::shared_ptr<WorldPipelineContext> worldPipelineContext;

    PipelineContext(std::shared_ptr<FrameworkContext> context, std::shared_ptr<Pipeline> pipeline);

    void fuseWorld();
};
