#pragma once

#include "core/render/texture_binding_snapshots.hpp"

#include "common/shared.hpp"
#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/render/modules/world/shader_pack/shader_pack.hpp"
#include "core/vulkan/all_core_vulkan.hpp"

#include "core/render/modules/world/world_module.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

class Framework;
class FrameworkContext;
class WorldPipeline;
struct WorldModuleContext;

struct RayTracingModuleContext;

class WorldPrepare;
struct WorldPrepareContext;

class RayTracingModule : public WorldModule, public SharedObject<RayTracingModule> {
    friend RayTracingModuleContext;

  public:
    constexpr static std::string_view NAME = "render_pipeline.module.ray_tracing.name";
    constexpr static uint32_t inputImageNum = 0;
    constexpr static uint32_t outputImageNum = 15;
    
    constexpr static std::string_view TARGET_RADIANCE = "out:radiance";
    constexpr static std::string_view TARGET_DIFFUSE_ALBEDO_METALLIC = "out:diffuse_albedo_metallic";
    constexpr static std::string_view TARGET_SPECULAR_ALBEDO = "out:specular_albedo";
    constexpr static std::string_view TARGET_NORMAL_ROUGHNESS = "out:normal_roughness";
    constexpr static std::string_view TARGET_MOTION_VECTOR = "out:motion_vector";
    constexpr static std::string_view TARGET_LINEAR_DEPTH = "out:linear_depth";
    constexpr static std::string_view TARGET_SPECULAR_HIT_DEPTH = "out:specular_hit_depth";
    constexpr static std::string_view TARGET_FIRST_HIT_DEPTH = "out:first_hit_depth";
    constexpr static std::string_view TARGET_FIRST_HIT_DIFFUSE_DIRECT_LIGHT = "out:first_hit_diffuse_direct_light";
    constexpr static std::string_view TARGET_FIRST_HIT_DIFFUSE_INDIRECT_LIGHT = "out:first_hit_diffuse_indirect_light";
    constexpr static std::string_view TARGET_FIRST_HIT_SPECULAR = "out:first_hit_specular";
    constexpr static std::string_view TARGET_FIRST_HIT_CLEAR = "out:first_hit_clear";
    constexpr static std::string_view TARGET_FIRST_HIT_BASE_EMISSION = "out:first_hit_base_emission";
    constexpr static std::string_view TARGET_FOG_IMAGE = "out:fog_image";
    constexpr static std::string_view TARGET_FIRST_HIT_REFRACTION = "out:first_hit_refraction";

    RayTracingModule();

    void init(std::shared_ptr<Framework> framework, std::shared_ptr<WorldPipeline> worldPipeline);

    bool setOrCreateInputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                std::vector<VkFormat> &formats,
                                uint32_t frameIndex) override;
    bool setOrCreateOutputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                 std::vector<VkFormat> &formats,
                                 uint32_t frameIndex) override;

    std::string getAttributes(const std::vector<std::string> &attributes, const std::string &language);

    void setAttributes(int attributeCount, std::vector<std::string> &attributeKVs) override;

    void build() override;

    std::vector<std::shared_ptr<WorldModuleContext>> &contexts() override;

    void
    bindTexture(std::shared_ptr<vk::Sampler> sampler, std::shared_ptr<vk::DeviceLocalImage> image, int index) override;

    void onResourceReload() override;

    void preClose() override;

  private:
    constexpr static uint32_t sharcCapacity = 1u << 23;
    constexpr static uint32_t sharcResolveWorkgroupSize = 64;
    constexpr static uint32_t executionLoopLimit = 1u << 16;

    struct SharcConfigData {
        uint32_t hashEntriesAddress[2];
        uint32_t lockAddress[2];
        uint32_t accumulationAddress[2];
        uint32_t resolvedAddress[2];
        glm::vec4 cameraPosition;
        glm::vec4 cameraPositionPrev;
        float sceneScale;
        float radianceScale;
        uint32_t accumulationFrameNum;
        uint32_t staleFrameNumMax;
        uint32_t capacity;
        uint32_t frameIndex;
        uint32_t enableAntiFireflyFilter;
        uint32_t useLockBuffer;
        uint32_t debugMode;
        uint32_t updateDownsampleFactor;
    };

    using ExecutionVariable = ShaderPack::ExecutionVariable;
    using ExecutionVariables = ShaderPack::ExecutionVariables;

    using PassVariant = std::variant<std::shared_ptr<FullScreenPass>,
                                     std::shared_ptr<RayTracingPass>,
                                     std::shared_ptr<ComputePass>>;

  private:
    static std::filesystem::path builtInShaderPackPath();
    static std::shared_ptr<vk::Shader> createShader(std::shared_ptr<vk::Device> device,
                                                    const std::filesystem::path &path,
                                                    VkShaderStageFlagBits stage,
                                                    const std::unordered_map<std::string, std::string> &definitions,
                                                    const std::vector<std::string> &includeDirectories,
                                                    const std::string &injectedSource);
    void loadShaderPack();
    void initDescriptorTables();
    void initRuntimeTextures();
    void initRuntimeBuffers();
    void refreshRuntimeBuffers(uint32_t frameIndex);
    void loadRuntimeResources();
    void initSharc();
    VkResult clearSharcStorage();
    void initPipelines();
    void initSBTs();
    void initContexts();
    void updateSharcConfig(uint32_t frameIndex);
    void initExecutionVariables();
    std::vector<ExpressionEvaluator::Variable> executionExpressionVariables() const;
    double evaluateNumericExpression(const std::string &expression,
                                     const ExecutionVariables &variables);
    std::optional<std::reference_wrapper<ShaderPackLoader::VariableConfig>> findExecutionVariableConfig(std::string_view name);
    std::optional<std::reference_wrapper<ShaderPack::RuntimeTexture>> findRuntimeTexture(std::string_view name);
    std::optional<std::reference_wrapper<ShaderPack::RuntimeBuffer>> findRuntimeBuffer(std::string_view name);
    std::shared_ptr<vk::DeviceLocalImage>
    findRuntimeVKTexture(ShaderPack::RuntimeTexture &runtimeTexture, uint32_t frameIndex);
    std::shared_ptr<vk::DeviceLocalBuffer>
    findRuntimeVKBuffer(ShaderPack::RuntimeBuffer &runtimeBuffer, uint32_t frameIndex);
    std::shared_ptr<vk::DeviceLocalImage> findTargetImage(const std::string &target, uint32_t frameIndex);
    std::vector<std::shared_ptr<vk::Framebuffer>> buildFramebuffers(std::shared_ptr<vk::DeviceLocalImage> image,
                                                                    std::shared_ptr<vk::RenderPass> renderPass);

    void addPassResourceBarriers(const std::vector<std::string> &inputImages,
                                 const std::vector<std::string> &inputBuffers,
                                 const std::vector<std::string> &outputImages,
                                 const std::vector<std::string> &outputBuffers,
                                 std::vector<vk::CommandBuffer::BufferMemoryBarrier> &bufferBarriers,
                                 std::vector<vk::CommandBuffer::ImageMemoryBarrier> &imageBarriers,
                                 uint32_t frameIndex,
                                 uint32_t queueIndex);
    void addFullScreenTargetBarrier(const std::string &target,
                                    std::vector<vk::CommandBuffer::ImageMemoryBarrier> &imageBarriers,
                                    uint32_t frameIndex,
                                    uint32_t queueIndex);

    void initFullScreenPassTargets(FullScreenPass &pass,
                                   std::shared_ptr<vk::Device> device,
                                   uint32_t frameCount);
    std::vector<ShaderPack::ShaderCreateInfo>
    collectFullScreenPassShaderRequests(const FullScreenPass &pass,
                                        const std::unordered_map<std::string, std::string> &definitions);
    void buildFullScreenPassPipelines(FullScreenPass &pass,
                                       std::shared_ptr<vk::Device> device,
                                       const std::vector<std::shared_ptr<vk::Shader>> &compiledShaders,
                                       size_t &shaderOffset);
    void renderFullScreenPass(const FullScreenPass &pass,
                              RayTracingModuleContext &context);

    std::vector<ShaderPack::ShaderCreateInfo>
    collectRayTracingPassShaderRequests(RayTracingPass &pass,
                                         const std::unordered_map<std::string, std::string> &definitions);
    void buildRayTracingPassPipelines(RayTracingPass &pass,
                                       std::shared_ptr<vk::Device> device,
                                       const std::vector<std::shared_ptr<vk::Shader>> &compiledShaders,
                                       size_t &shaderOffset);
    void renderSharcUpdateAndResolve(RayTracingPass &pass,
                                     RayTracingModuleContext &context,
                                     const ExecutionVariables &variables);
    void renderRayTracingPass(RayTracingPass &pass,
                              RayTracingModuleContext &context,
                              const ExecutionVariables &variables);

    std::vector<ShaderPack::ShaderCreateInfo>
    collectComputePassShaderRequests(const ComputePass &pass,
                                     const std::unordered_map<std::string, std::string> &definitions);
    void buildComputePassPipelines(ComputePass &pass,
                                    std::shared_ptr<vk::Device> device,
                                    const std::vector<std::shared_ptr<vk::Shader>> &compiledShaders,
                                    size_t &shaderOffset);
    void renderComputePass(const ComputePass &pass,
                           RayTracingModuleContext &context,
                           const ExecutionVariables &variables);
    size_t shaderRequestCountForPass(PassVariant &passVariant);
    void uploadStaticRayTracingPassSbts(std::shared_ptr<vk::Device> device);

  private:
    std::shared_ptr<vk::Shader> fullScreenVertexShader_;

    std::vector<std::shared_ptr<vk::DescriptorTable>> rayTracingDescriptorTables_;
    mcvr::TextureBindingSnapshots<std::pair<std::shared_ptr<vk::Sampler>, std::shared_ptr<vk::Image>>,
                                  vk::DescriptorTable> textureBindings_;
    std::shared_ptr<vk::DescriptorTable> textureSnapshot();

    std::shared_ptr<ShaderPack> shaderPack_;
    std::vector<PassVariant> passes_;
    std::unordered_map<std::string, PassVariant> passNameToPass_;
    std::shared_ptr<RayTracingPass> sharcUpdatePass_;

    std::shared_ptr<vk::DeviceLocalBuffer> sharcHashEntriesBuffer_;
    std::shared_ptr<vk::DeviceLocalBuffer> sharcLockBuffer_;
    std::shared_ptr<vk::DeviceLocalBuffer> sharcAccumulationBuffer_;
    std::shared_ptr<vk::DeviceLocalBuffer> sharcResolvedBuffer_;
    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> sharcConfigBuffers_;

    glm::dvec3 sharcPrevCameraPos_ = glm::dvec3(0.0);
    uint32_t sharcFrameIndex_ = 0;
    bool isFirstSharcFrame_ = true;
    bool hasSharcRuntime_ = false;

    bool isJitterEnabled_ = true;
    bool isSharcEnabled_ = true;
    uint32_t sharcDebugMode_ = 0;
    std::string shaderPackPath_;
    float sharcSceneScale_ = 64.0f;
    uint32_t sharcAccumulationFrameNum_ = 64;
    uint32_t sharcStaleFrameNumMax_ = 256;
    uint32_t sharcUpdateDownsampleFactor_ = 5;

    std::unordered_map<std::string, ShaderPackLoader::VariableConfig> executionVariableConfigs_;
    std::unordered_map<std::string, std::string> globalVariables_;
    std::unordered_map<std::string, std::string> staticAttributes_;

    std::vector<std::shared_ptr<vk::DeviceLocalImage>> hdrNoisyOutputImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> diffuseAlbedoImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> specularAlbedoImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> normalRoughnessImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> motionVectorImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> linearDepthImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> specularHitDepthImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> firstHitDepthImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> firstHitDiffuseDirectLightImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> firstHitDiffuseIndirectLightImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> firstHitSpecularImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> firstHitClearImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> firstHitBaseEmissionImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> fogImages_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> firstHitRefractionImages_;

    std::shared_ptr<WorldPrepare> worldPrepare_;

    std::vector<std::shared_ptr<WorldModuleContext>> contexts_;
};

struct RayTracingModuleContext : public WorldModuleContext, SharedObject<RayTracingModuleContext> {
    std::weak_ptr<RayTracingModule> rayTracingModule;

    std::shared_ptr<vk::DescriptorTable> rayTracingDescriptorTable;

    std::shared_ptr<vk::DeviceLocalImage> hdrNoisyOutputImage;
    std::shared_ptr<vk::DeviceLocalImage> diffuseAlbedoImage;
    std::shared_ptr<vk::DeviceLocalImage> specularAlbedoImage;
    std::shared_ptr<vk::DeviceLocalImage> normalRoughnessImage;
    std::shared_ptr<vk::DeviceLocalImage> motionVectorImage;
    std::shared_ptr<vk::DeviceLocalImage> linearDepthImage;
    std::shared_ptr<vk::DeviceLocalImage> specularHitDepthImage;
    std::shared_ptr<vk::DeviceLocalImage> firstHitDepthImage;
    std::shared_ptr<vk::DeviceLocalImage> firstHitDiffuseDirectLightImage;
    std::shared_ptr<vk::DeviceLocalImage> firstHitDiffuseIndirectLightImage;
    std::shared_ptr<vk::DeviceLocalImage> firstHitSpecularImage;
    std::shared_ptr<vk::DeviceLocalImage> firstHitClearImage;
    std::shared_ptr<vk::DeviceLocalImage> firstHitBaseEmissionImage;
    std::shared_ptr<vk::DeviceLocalImage> fogImage;
    std::shared_ptr<vk::DeviceLocalImage> firstHitRefractionImage;

    std::shared_ptr<WorldPrepareContext> worldPrepareContext;

    RayTracingModuleContext(std::shared_ptr<FrameworkContext> frameworkContext,
                            std::shared_ptr<WorldPipelineContext> worldPipelineContext,
                            std::shared_ptr<RayTracingModule> rayTracingModule);

    void render() override;
};
