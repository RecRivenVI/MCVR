#include "core/logging.hpp"
#include "core/diagnostics/frame_profile.hpp"
#include "core/failure_state.hpp"
#include "core/diagnostics/device_loss_trace.hpp"
#include "core/render/scene_scope.hpp"
#include "core/render/modules/world/ray_tracing/ray_tracing_module.hpp"

#include "core/render/buffers.hpp"
#include "core/render/chunks.hpp"
#include "core/render/modules/world/ray_tracing/submodules/world_prepare.hpp"
#include "core/render/modules/world/shader_pack/shader_pack.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/world.hpp"
#include "core/util/parallel.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

std::filesystem::path RayTracingModule::builtInShaderPackPath() {
    return Renderer::folderPath / "shaders/world/ray_tracing/vanilla-pt.zip";
}

std::shared_ptr<vk::Shader>
RayTracingModule::createShader(std::shared_ptr<vk::Device> device,
                               const std::filesystem::path &path,
                               VkShaderStageFlagBits stage,
                               const std::unordered_map<std::string, std::string> &definitions,
                               const std::vector<std::string> &includeDirectories,
                               const std::string &injectedSource) {
    return vk::Shader::create(device, path.string(), stage, definitions, includeDirectories, injectedSource);
}

void RayTracingModule::addPassResourceBarriers(
    const std::vector<std::string> &inputImages,
    const std::vector<std::string> &inputBuffers,
    const std::vector<std::string> &outputImages,
    const std::vector<std::string> &outputBuffers,
    std::vector<vk::CommandBuffer::BufferMemoryBarrier> &bufferBarriers,
    std::vector<vk::CommandBuffer::ImageMemoryBarrier> &imageBarriers,
    uint32_t frameIndex,
    uint32_t queueIndex) {
    auto addImageBarrier = [&](const std::shared_ptr<vk::DeviceLocalImage> &image, VkImageLayout newLayout) {
        if (image == nullptr) { return; }

        VkPipelineStageFlags2 dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                              VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkAccessFlags2 dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        switch (newLayout) {
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_GENERAL:
                dstStageMask |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                break;
            default:
                throw std::runtime_error("unsupported barrier layout");
        }

        imageBarriers.push_back({
            .srcStageMask = image->imageLayout() == VK_IMAGE_LAYOUT_UNDEFINED ?
                                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT :
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                                    VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = image->imageLayout() == VK_IMAGE_LAYOUT_UNDEFINED ?
                                 0 :
                                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = dstStageMask,
            .dstAccessMask = dstAccessMask,
            .oldLayout = image->imageLayout(),
            .newLayout = newLayout,
            .srcQueueFamilyIndex = queueIndex,
            .dstQueueFamilyIndex = queueIndex,
            .image = image,
            .subresourceRange = image->fullSubresourceRange(),
        });
        image->imageLayout() = newLayout;
    };

    auto addBufferBarrier = [&](const std::shared_ptr<vk::DeviceLocalBuffer> &buffer, bool writable) {
        if (buffer == nullptr) { return; }

        const VkPipelineStageFlags2 shaderStages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                    VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        bufferBarriers.push_back({
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = shaderStages,
            .dstAccessMask = writable ? (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)
                                       : VK_ACCESS_2_SHADER_READ_BIT,
            .srcQueueFamilyIndex = queueIndex,
            .dstQueueFamilyIndex = queueIndex,
            .buffer = buffer,
        });
    };

    std::unordered_map<std::string, VkImageLayout> resourceLayouts;
    resourceLayouts.reserve(inputImages.size() + outputImages.size());
    std::unordered_map<std::string, bool> bufferAccess;
    bufferAccess.reserve(inputBuffers.size() + outputBuffers.size());

    for (const std::string &input : inputImages) {
        auto runtimeTexture = findRuntimeTexture(input);
        if (runtimeTexture.has_value() && runtimeTexture->get().config.imported) {
            resourceLayouts.emplace(input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            continue;
        }
        if (runtimeTexture.has_value() && !runtimeTexture->get().config.storageBinding.has_value() &&
            runtimeTexture->get().config.sampledBinding.has_value()) {
            resourceLayouts.emplace(input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            continue;
        }
        resourceLayouts.emplace(input, VK_IMAGE_LAYOUT_GENERAL);
    }

    for (const std::string &input : inputBuffers) { bufferAccess.emplace(input, false); }

    for (const std::string &output : outputImages) {
        auto runtimeTexture = findRuntimeTexture(output);
        if (runtimeTexture.has_value() && runtimeTexture->get().config.imported) {
            throw std::runtime_error("pass output cannot be an imported texture: " + output);
        }
        resourceLayouts[output] = VK_IMAGE_LAYOUT_GENERAL;
    }

    for (const std::string &output : outputBuffers) { bufferAccess[output] = true; }

    for (const auto &[resource, layout] : resourceLayouts) {
        if (auto runtimeTexture = findRuntimeTexture(resource);
            runtimeTexture.has_value() && runtimeTexture->get().config.imported) {
            addImageBarrier(runtimeTexture->get().importedImage, layout);
            continue;
        }

        std::shared_ptr<vk::DeviceLocalImage> image = findTargetImage(resource, frameIndex);
        if (image == nullptr) { throw std::runtime_error("unknown pass resource: " + resource); }
        addImageBarrier(image, layout);
    }

    for (const auto &[resource, writable] : bufferAccess) {
        auto runtimeBuffer = findRuntimeBuffer(resource);
        if (!runtimeBuffer.has_value()) { throw std::runtime_error("unknown pass buffer resource: " + resource); }
        addBufferBarrier(findRuntimeVKBuffer(runtimeBuffer->get(), frameIndex), writable);
    }
}

void RayTracingModule::addFullScreenTargetBarrier(
    const std::string &target,
    std::vector<vk::CommandBuffer::ImageMemoryBarrier> &imageBarriers,
    uint32_t frameIndex,
    uint32_t queueIndex) {
    auto runtimeTexture = findRuntimeTexture(target);
    if (runtimeTexture.has_value() && runtimeTexture->get().config.imported) {
        throw std::runtime_error("pass output cannot be an imported texture: " + target);
    }

    std::shared_ptr<vk::DeviceLocalImage> image = findTargetImage(target, frameIndex);
    if (image == nullptr) { throw std::runtime_error("unknown pass resource: " + target); }

    imageBarriers.push_back({
        .srcStageMask = image->imageLayout() == VK_IMAGE_LAYOUT_UNDEFINED ?
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT :
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = image->imageLayout() == VK_IMAGE_LAYOUT_UNDEFINED ?
                             0 :
                             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = image->imageLayout(),
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = queueIndex,
        .dstQueueFamilyIndex = queueIndex,
        .image = image,
        .subresourceRange = image->fullSubresourceRange(),
    });
    image->imageLayout() = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

RayTracingModule::RayTracingModule() {}

void RayTracingModule::init(std::shared_ptr<Framework> framework, std::shared_ptr<WorldPipeline> worldPipeline) {
    WorldModule::init(framework, worldPipeline);

    uint32_t size = framework->recordingContextCount();

    hdrNoisyOutputImages_.resize(size);
    diffuseAlbedoImages_.resize(size);
    specularAlbedoImages_.resize(size);
    normalRoughnessImages_.resize(size);
    motionVectorImages_.resize(size);
    linearDepthImages_.resize(size);
    specularHitDepthImages_.resize(size);
    firstHitDepthImages_.resize(size);
    firstHitDiffuseDirectLightImages_.resize(size);
    firstHitDiffuseIndirectLightImages_.resize(size);
    firstHitSpecularImages_.resize(size);
    firstHitClearImages_.resize(size);
    firstHitBaseEmissionImages_.resize(size);
    fogImages_.resize(size);
    firstHitRefractionImages_.resize(size);

    worldPrepare_ = WorldPrepare::create(framework, shared_from_this());
}

bool RayTracingModule::setOrCreateInputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                              std::vector<VkFormat> &formats,
                                              uint32_t frameIndex) {
    return true;
}

bool RayTracingModule::setOrCreateOutputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                               std::vector<VkFormat> &formats,
                                               uint32_t frameIndex) {
    uint32_t width = 0;
    uint32_t height = 0;
    bool set = false;
    for (auto &image : images) {
        if (image != nullptr) {
            if (!set) {
                width = image->width();
                height = image->height();
                set = true;
            } else if (image->width() != width || image->height() != height) {
                return false;
            }
        }
    }

    if (!set) { return false; }

    auto framework = framework_.lock();
    for (int i = 0; i < images.size(); i++) {
        if (images[i] == nullptr) {
            images[i] = vk::DeviceLocalImage::create(
                framework->device(), framework->vma(), false, width, height, 1, formats[i],
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        }
    }

    hdrNoisyOutputImages_[frameIndex] = images[0];
    diffuseAlbedoImages_[frameIndex] = images[1];
    specularAlbedoImages_[frameIndex] = images[2];
    normalRoughnessImages_[frameIndex] = images[3];
    motionVectorImages_[frameIndex] = images[4];
    linearDepthImages_[frameIndex] = images[5];
    specularHitDepthImages_[frameIndex] = images[6];
    firstHitDepthImages_[frameIndex] = images[7];
    firstHitDiffuseDirectLightImages_[frameIndex] = images[8];
    firstHitDiffuseIndirectLightImages_[frameIndex] = images[9];
    firstHitSpecularImages_[frameIndex] = images[10];
    firstHitClearImages_[frameIndex] = images[11];
    firstHitBaseEmissionImages_[frameIndex] = images[12];
    fogImages_[frameIndex] = images[13];
    firstHitRefractionImages_[frameIndex] = images[14];

    return true;
}

std::string RayTracingModule::getAttributes(const std::vector<std::string> &attributes, const std::string &language) {
    std::string shaderPackPath;
    for (size_t i = 0; i + 1 < attributes.size(); i += 2) {
        if (attributes[i] == "render_pipeline.module.ray_tracing.attribute.shader_pack_path") {
            shaderPackPath = attributes[i + 1];
            break;
        }
    }
    ShaderPackLoader::LoadResult loadResult =
        ShaderPackLoader::load(shaderPackPath.empty() ? std::filesystem::path{} : std::filesystem::path(shaderPackPath),
                               builtInShaderPackPath(), language);

    json result = json::object();
    result["attributes"] = json::array();
    result["translations"] = json::object();
    if (!loadResult.success) { return result.dump(); }

    for (const auto &attribute : loadResult.shaderPack.attributes) {
        result["attributes"].push_back({
            {"name", attribute.name},
            {"type", attribute.type},
            {"default_value", attribute.defaultValue},
        });
    }
    for (const auto &[key, value] : loadResult.shaderPack.translations) { result["translations"][key] = value; }
    return result.dump();
}

void RayTracingModule::setAttributes(int attributeCount, std::vector<std::string> &attributeKVs) {
    staticAttributes_.clear();
    for (int i = 0; i < attributeCount; i++) {
        const std::string &key = attributeKVs[2 * i];
        const std::string &value = attributeKVs[2 * i + 1];
        staticAttributes_[key] = value;
    }

    isJitterEnabled_ = true;
    isSharcEnabled_ = true;
    sharcDebugMode_ = 0;
    shaderPackPath_.clear();
    sharcSceneScale_ = 64.0f;
    sharcAccumulationFrameNum_ = 64;
    sharcStaleFrameNumMax_ = 256;
    sharcUpdateDownsampleFactor_ = 5;

    for (const auto &[key, value] : staticAttributes_) {
        if (key == "render_pipeline.module.ray_tracing.attribute.use_jitter") {
            isJitterEnabled_ = value == "render_pipeline.true";
        } else if (key == "render_pipeline.module.ray_tracing.attribute.use_sharc") {
            isSharcEnabled_ = value == "render_pipeline.true";
        } else if (key == "render_pipeline.module.ray_tracing.attribute.sharc_debug_mode") {
            if (value == "off" || value == "0" || value == "render_pipeline.false") {
                sharcDebugMode_ = 0;
            } else if (value == "hash_grid" || value == "1") {
                sharcDebugMode_ = 1;
            } else if (value == "occupancy" || value == "2") {
                sharcDebugMode_ = 2;
            } else if (value == "heatmap" || value == "3") {
                sharcDebugMode_ = 3;
            } else {
                try {
                    sharcDebugMode_ = static_cast<uint32_t>(std::max(0, std::stoi(value)));
                } catch (...) { sharcDebugMode_ = 0; }
            }
        } else if (key == "render_pipeline.module.ray_tracing.attribute.shader_pack_path") {
            shaderPackPath_ = value;
        } else if (key == "render_pipeline.module.ray_tracing.attribute.sharc_scene_scale") {
            try {
                sharcSceneScale_ = std::max(0.001f, std::stof(value));
            } catch (...) {}
        } else if (key == "render_pipeline.module.ray_tracing.attribute.sharc_accumulation_frame_num") {
            try {
                sharcAccumulationFrameNum_ = std::max(1u, static_cast<uint32_t>(std::max(0, std::stoi(value))));
            } catch (...) {}
        } else if (key == "render_pipeline.module.ray_tracing.attribute.sharc_stale_frame_num_max") {
            try {
                sharcStaleFrameNumMax_ = std::max(8u, static_cast<uint32_t>(std::max(0, std::stoi(value))));
            } catch (...) {}
        } else if (key == "render_pipeline.module.ray_tracing.attribute.sharc_update_downsample_factor") {
            try {
                sharcUpdateDownsampleFactor_ = std::max(1u, static_cast<uint32_t>(std::max(0, std::stoi(value))));
            } catch (...) {}
        }
    }
    Renderer::instance().buffers()->setUseJitter(isJitterEnabled_);
}

void RayTracingModule::loadShaderPack() {
    auto worldPipeline = worldPipeline_.lock();
    shaderPack_ = worldPipeline != nullptr ? worldPipeline->shaderPack() : nullptr;
    if (shaderPack_ == nullptr) {
        throw std::runtime_error("Ray tracing shader-pack runtime is unavailable");
    }
    hasSharcRuntime_ = shaderPack_->hasSharcRuntime();
}

void RayTracingModule::initExecutionVariables() {
    executionVariableConfigs_.clear();
    globalVariables_.clear();

    if (shaderPack_ == nullptr) { return; }
    shaderPack_->copyStageExecutionState(ShaderPackLoader::Stage::RayTracing, executionVariableConfigs_,
                                         globalVariables_);
}

std::vector<ExpressionEvaluator::Variable> RayTracingModule::executionExpressionVariables() const {
    std::vector<ExpressionEvaluator::Variable> variables;
    variables.reserve(8);

    const uint32_t renderWidth = hdrNoisyOutputImages_.empty() || hdrNoisyOutputImages_[0] == nullptr ?
                                     0u :
                                     hdrNoisyOutputImages_[0]->width();
    const uint32_t renderHeight = hdrNoisyOutputImages_.empty() || hdrNoisyOutputImages_[0] == nullptr ?
                                      0u :
                                      hdrNoisyOutputImages_[0]->height();
    variables.push_back({.name = "RENDER_WIDTH", .value = static_cast<double>(renderWidth)});
    variables.push_back({.name = "RENDER_HEIGHT", .value = static_cast<double>(renderHeight)});

    auto world = Renderer::instance().world();
    auto chunks = world == nullptr ? nullptr : world->chunks();
    if (chunks != nullptr) {
        const glm::ivec4 chunkGridInfo = chunks->chunkGridInfo();
        variables.push_back({.name = "CHUNK_SIZE_X", .value = static_cast<double>(chunkGridInfo.x)});
        variables.push_back({.name = "CHUNK_SIZE_Y", .value = static_cast<double>(chunkGridInfo.y)});
        variables.push_back({.name = "CHUNK_SIZE_Z", .value = static_cast<double>(chunkGridInfo.z)});
        variables.push_back({.name = "CHUNK_BOTTOM_SECTION_COORD", .value = static_cast<double>(chunkGridInfo.w)});
        variables.push_back({.name = "CHUNK_NUM", .value = static_cast<double>(chunks->chunks().size())});
    }
    return variables;
}

double RayTracingModule::evaluateNumericExpression(const std::string &expression,
                                                   const RayTracingModule::ExecutionVariables &variables) {
    return shaderPack_->evaluateNumericExpression(ShaderPackLoader::Stage::RayTracing, expression, variables,
                                                  executionExpressionVariables(), true);
}

std::optional<std::reference_wrapper<ShaderPackLoader::VariableConfig>>
RayTracingModule::findExecutionVariableConfig(std::string_view name) {
    auto executionIter = executionVariableConfigs_.find(std::string(name));
    if (executionIter != executionVariableConfigs_.end()) { return std::ref(executionIter->second); }
    return std::nullopt;
}

void RayTracingModule::initDescriptorTables() {
    auto framework = framework_.lock();
    uint32_t size = framework->recordingContextCount();
    VkShaderStageFlags runtimeTextureStageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_VERTEX_BIT |
        VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    rayTracingDescriptorTables_.resize(size);

    for (uint32_t frameIndex = 0; frameIndex < size; frameIndex++) {
        vk::DescriptorTableBuilder builder;

        auto &set0 = builder.beginDescriptorLayoutSet();
        auto &set0Bindings = set0.beginDescriptorLayoutSetBinding();
        set0Bindings.defineDescriptorLayoutSetBinding({
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 4096,
            .stageFlags = runtimeTextureStageFlags,
        });
        set0Bindings.endDescriptorLayoutSetBinding();
        set0.endDescriptorLayoutSet();

        auto &set1 = builder.beginDescriptorLayoutSet();
        auto &set1Bindings = set1.beginDescriptorLayoutSetBinding();
        set1Bindings
            .defineDescriptorLayoutSetBinding({
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = runtimeTextureStageFlags,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                              VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 3,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                              VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 4,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 5,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 6,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 7,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = runtimeTextureStageFlags,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 8,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                              VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 9,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = runtimeTextureStageFlags,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 10,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 11,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            });
        set1Bindings.endDescriptorLayoutSetBinding();
        set1.endDescriptorLayoutSet();

        auto &set2 = builder.beginDescriptorLayoutSet();
        auto &set2Bindings = set2.beginDescriptorLayoutSetBinding();
        set2Bindings
            .defineDescriptorLayoutSetBinding({
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = runtimeTextureStageFlags,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                              VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                              VK_SHADER_STAGE_COMPUTE_BIT,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = runtimeTextureStageFlags,
            });
        set2Bindings.endDescriptorLayoutSetBinding();
        set2.endDescriptorLayoutSet();

        auto &set3 = builder.beginDescriptorLayoutSet();
        auto &set3Bindings = set3.beginDescriptorLayoutSetBinding();
        for (uint32_t binding = 0; binding < outputImageNum; binding++) {
            set3Bindings.defineDescriptorLayoutSetBinding({
                .binding = binding,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                              VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                              VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
            });
        }
        set3Bindings.endDescriptorLayoutSetBinding();
        set3.endDescriptorLayoutSet();

        auto &set4 = builder.beginDescriptorLayoutSet();
        auto &set4Bindings = set4.beginDescriptorLayoutSetBinding();
        set4Bindings
            .defineDescriptorLayoutSetBinding({
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 3,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT,
            })
            .defineDescriptorLayoutSetBinding({
                .binding = 4,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT,
            });
        set4Bindings.endDescriptorLayoutSetBinding();
        set4.endDescriptorLayoutSet();

        shaderPack_->defineRuntimeResourceDescriptorSet(builder, runtimeTextureStageFlags, runtimeTextureStageFlags,
                                                        runtimeTextureStageFlags);
        shaderPack_->defineExecutionDescriptorSet(builder, ShaderPackLoader::Stage::RayTracing,
                                                  runtimeTextureStageFlags | VK_SHADER_STAGE_COMPUTE_BIT);

        rayTracingDescriptorTables_[frameIndex] = builder.build(framework->device());

        rayTracingDescriptorTables_[frameIndex]->bindImage(hdrNoisyOutputImages_[frameIndex], VK_IMAGE_LAYOUT_GENERAL,
                                                           3, 0);
        rayTracingDescriptorTables_[frameIndex]->bindImage(diffuseAlbedoImages_[frameIndex], VK_IMAGE_LAYOUT_GENERAL, 3,
                                                           1);
        rayTracingDescriptorTables_[frameIndex]->bindImage(specularAlbedoImages_[frameIndex], VK_IMAGE_LAYOUT_GENERAL,
                                                           3, 2);
        rayTracingDescriptorTables_[frameIndex]->bindImage(normalRoughnessImages_[frameIndex], VK_IMAGE_LAYOUT_GENERAL,
                                                           3, 3);
        rayTracingDescriptorTables_[frameIndex]->bindImage(motionVectorImages_[frameIndex], VK_IMAGE_LAYOUT_GENERAL, 3,
                                                           4);
        rayTracingDescriptorTables_[frameIndex]->bindImage(linearDepthImages_[frameIndex], VK_IMAGE_LAYOUT_GENERAL, 3,
                                                           5);
        rayTracingDescriptorTables_[frameIndex]->bindImage(specularHitDepthImages_[frameIndex], VK_IMAGE_LAYOUT_GENERAL,
                                                           3, 6);
        rayTracingDescriptorTables_[frameIndex]->bindImage(firstHitDepthImages_[frameIndex], VK_IMAGE_LAYOUT_GENERAL, 3,
                                                           7);
        rayTracingDescriptorTables_[frameIndex]->bindImage(firstHitDiffuseDirectLightImages_[frameIndex],
                                                           VK_IMAGE_LAYOUT_GENERAL, 3, 8);
        rayTracingDescriptorTables_[frameIndex]->bindImage(firstHitDiffuseIndirectLightImages_[frameIndex],
                                                           VK_IMAGE_LAYOUT_GENERAL, 3, 9);
        rayTracingDescriptorTables_[frameIndex]->bindImage(firstHitSpecularImages_[frameIndex], VK_IMAGE_LAYOUT_GENERAL,
                                                           3, 10);
        rayTracingDescriptorTables_[frameIndex]->bindImage(firstHitClearImages_[frameIndex], VK_IMAGE_LAYOUT_GENERAL, 3,
                                                           11);
        rayTracingDescriptorTables_[frameIndex]->bindImage(firstHitBaseEmissionImages_[frameIndex],
                                                           VK_IMAGE_LAYOUT_GENERAL, 3, 12);
        rayTracingDescriptorTables_[frameIndex]->bindImage(fogImages_[frameIndex], VK_IMAGE_LAYOUT_GENERAL, 3, 13);
        rayTracingDescriptorTables_[frameIndex]->bindImage(firstHitRefractionImages_[frameIndex],
                                                           VK_IMAGE_LAYOUT_GENERAL, 3, 14);
    }
}

void RayTracingModule::initRuntimeTextures() {
    auto framework = framework_.lock();
    auto device = framework->device();

    fullScreenVertexShader_ =
        vk::Shader::create(device, (Renderer::folderPath / "shaders/full_screen_vert.spv").string());
    if (shaderPack_ != nullptr && !hdrNoisyOutputImages_.empty() && hdrNoisyOutputImages_[0] != nullptr) {
        shaderPack_->setRuntimeResourceExpressionVariables(executionExpressionVariables());
        shaderPack_->ensureRuntimeResources(hdrNoisyOutputImages_[0]->width(), hdrNoisyOutputImages_[0]->height());
    }
}

void RayTracingModule::initRuntimeBuffers() {
    if (shaderPack_ != nullptr) {
        shaderPack_->setRuntimeResourceExpressionVariables(executionExpressionVariables());
        shaderPack_->refreshRuntimeBuffers();
    }
}

void RayTracingModule::refreshRuntimeBuffers(uint32_t frameIndex) {
    mcvr::profile::Scope profile("pt.runtime-buffers");
    if (shaderPack_ == nullptr) { return; }
    shaderPack_->setRuntimeResourceExpressionVariables(executionExpressionVariables());
    shaderPack_->refreshRuntimeBuffers();
    shaderPack_->bindRuntimeResources(rayTracingDescriptorTables_[frameIndex], 5, frameIndex);
}

void RayTracingModule::loadRuntimeResources() {
    auto framework = framework_.lock();
    for (uint32_t frameIndex = 0; frameIndex < framework->recordingContextCount(); frameIndex++) {
        shaderPack_->bindRuntimeResources(rayTracingDescriptorTables_[frameIndex], 5, frameIndex);
    }
}

std::vector<std::shared_ptr<vk::Framebuffer>>
RayTracingModule::buildFramebuffers(std::shared_ptr<vk::DeviceLocalImage> image,
                                    std::shared_ptr<vk::RenderPass> renderPass) {
    auto framework = framework_.lock();
    std::vector<std::shared_ptr<vk::Framebuffer>> framebuffers;
    if (image->layer() == 1) {
        framebuffers.push_back(vk::FramebufferBuilder{}.beginAttachment().defineAttachment(image).endAttachment().build(
            framework->device(), renderPass));
        return framebuffers;
    }

    framebuffers.resize(image->layer());
    for (uint32_t layerIndex = 0; layerIndex < image->layer(); layerIndex++) {
        framebuffers[layerIndex] = vk::FramebufferBuilder{}
                                       .beginAttachment()
                                       .defineAttachment(image, layerIndex + 1)
                                       .endAttachment()
                                       .build(framework->device(), renderPass);
    }
    return framebuffers;
}

std::shared_ptr<vk::DeviceLocalImage> RayTracingModule::findRuntimeVKTexture(ShaderPack::RuntimeTexture &runtimeTexture,
                                                                             uint32_t frameIndex) {
    return shaderPack_->findRuntimeVKTexture(runtimeTexture, frameIndex);
}

std::shared_ptr<vk::DeviceLocalBuffer> RayTracingModule::findRuntimeVKBuffer(ShaderPack::RuntimeBuffer &runtimeBuffer,
                                                                             uint32_t frameIndex) {
    return shaderPack_->findRuntimeVKBuffer(runtimeBuffer, frameIndex);
}

std::shared_ptr<vk::DeviceLocalImage> RayTracingModule::findTargetImage(const std::string &target,
                                                                        uint32_t frameIndex) {
    if (target == TARGET_RADIANCE) return hdrNoisyOutputImages_[frameIndex];
    if (target == TARGET_DIFFUSE_ALBEDO_METALLIC) return diffuseAlbedoImages_[frameIndex];
    if (target == TARGET_SPECULAR_ALBEDO) return specularAlbedoImages_[frameIndex];
    if (target == TARGET_NORMAL_ROUGHNESS) return normalRoughnessImages_[frameIndex];
    if (target == TARGET_MOTION_VECTOR) return motionVectorImages_[frameIndex];
    if (target == TARGET_LINEAR_DEPTH) return linearDepthImages_[frameIndex];
    if (target == TARGET_SPECULAR_HIT_DEPTH) return specularHitDepthImages_[frameIndex];
    if (target == TARGET_FIRST_HIT_DEPTH) return firstHitDepthImages_[frameIndex];
    if (target == TARGET_FIRST_HIT_DIFFUSE_DIRECT_LIGHT) return firstHitDiffuseDirectLightImages_[frameIndex];
    if (target == TARGET_FIRST_HIT_DIFFUSE_INDIRECT_LIGHT) return firstHitDiffuseIndirectLightImages_[frameIndex];
    if (target == TARGET_FIRST_HIT_SPECULAR) return firstHitSpecularImages_[frameIndex];
    if (target == TARGET_FIRST_HIT_CLEAR) return firstHitClearImages_[frameIndex];
    if (target == TARGET_FIRST_HIT_BASE_EMISSION) return firstHitBaseEmissionImages_[frameIndex];
    if (target == TARGET_FOG_IMAGE) return fogImages_[frameIndex];
    if (target == TARGET_FIRST_HIT_REFRACTION) return firstHitRefractionImages_[frameIndex];

    if (auto runtimeTexture = findRuntimeTexture(target); runtimeTexture.has_value()) {
        if (runtimeTexture->get().config.imported) { return nullptr; }
        return findRuntimeVKTexture(runtimeTexture->get(), frameIndex);
    }
    return nullptr;
}

std::optional<std::reference_wrapper<ShaderPack::RuntimeTexture>>
RayTracingModule::findRuntimeTexture(std::string_view name) {
    if (shaderPack_ == nullptr) { return std::nullopt; }
    return shaderPack_->findRuntimeTexture(name);
}

std::optional<std::reference_wrapper<ShaderPack::RuntimeBuffer>>
RayTracingModule::findRuntimeBuffer(std::string_view name) {
    if (shaderPack_ == nullptr) { return std::nullopt; }
    return shaderPack_->findRuntimeBuffer(name);
}

void RayTracingModule::initSharc() {
    sharcConfigBuffers_.clear();
    sharcHashEntriesBuffer_ = nullptr;
    sharcLockBuffer_ = nullptr;
    sharcAccumulationBuffer_ = nullptr;
    sharcResolvedBuffer_ = nullptr;

    if (!hasSharcRuntime_) { return; }

    auto framework = framework_.lock();
    auto device = framework->device();
    auto vma = framework->vma();
    uint32_t size = framework->recordingContextCount();

    sharcConfigBuffers_.resize(size);

    const VkBufferUsageFlags sharcStorageUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    sharcHashEntriesBuffer_ =
        vk::DeviceLocalBuffer::create(vma, device, false, static_cast<size_t>(sharcCapacity) * sizeof(uint64_t),
                                      sharcStorageUsage, 0, VMA_MEMORY_USAGE_GPU_ONLY);
    sharcLockBuffer_ =
        vk::DeviceLocalBuffer::create(vma, device, false, static_cast<size_t>(sharcCapacity) * sizeof(uint32_t),
                                      sharcStorageUsage, 0, VMA_MEMORY_USAGE_GPU_ONLY);
    sharcAccumulationBuffer_ =
        vk::DeviceLocalBuffer::create(vma, device, false, static_cast<size_t>(sharcCapacity) * sizeof(uint32_t) * 4,
                                      sharcStorageUsage, 0, VMA_MEMORY_USAGE_GPU_ONLY);
    sharcResolvedBuffer_ =
        vk::DeviceLocalBuffer::create(vma, device, false, static_cast<size_t>(sharcCapacity) * sizeof(uint32_t) * 4,
                                      sharcStorageUsage, 0, VMA_MEMORY_USAGE_GPU_ONLY);

    if (clearSharcStorage() != VK_SUCCESS) { return; }

    for (uint32_t i = 0; i < size; i++) {
        sharcConfigBuffers_[i] =
            vk::HostVisibleBuffer::create(vma, device, sizeof(SharcConfigData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        rayTracingDescriptorTables_[i]->bindBuffer(sharcConfigBuffers_[i], 4, 0);
        rayTracingDescriptorTables_[i]->bindBuffer(sharcHashEntriesBuffer_, 4, 1);
        rayTracingDescriptorTables_[i]->bindBuffer(sharcAccumulationBuffer_, 4, 2);
        rayTracingDescriptorTables_[i]->bindBuffer(sharcResolvedBuffer_, 4, 3);
        rayTracingDescriptorTables_[i]->bindBuffer(sharcLockBuffer_, 4, 4);
    }
}

VkResult RayTracingModule::clearSharcStorage() {
    if (!hasSharcRuntime_ || sharcHashEntriesBuffer_ == nullptr || sharcLockBuffer_ == nullptr ||
        sharcAccumulationBuffer_ == nullptr || sharcResolvedBuffer_ == nullptr) {
        return VK_SUCCESS;
    }

    auto framework = framework_.lock();
    auto device = framework->device();
    auto clearCommandPool = vk::CommandPool::create(framework->physicalDevice(), device);
    auto clearCommandBuffer = vk::CommandBuffer::create(device, clearCommandPool);
    clearCommandBuffer->begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    vkCmdFillBuffer(clearCommandBuffer->vkCommandBuffer(), sharcHashEntriesBuffer_->vkBuffer(), 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(clearCommandBuffer->vkCommandBuffer(), sharcLockBuffer_->vkBuffer(), 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(clearCommandBuffer->vkCommandBuffer(), sharcAccumulationBuffer_->vkBuffer(), 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(clearCommandBuffer->vkCommandBuffer(), sharcResolvedBuffer_->vkBuffer(), 0, VK_WHOLE_SIZE, 0);
    clearCommandBuffer->end();
    VkResult result = clearCommandBuffer->submitMainQueueIndividual(device);
    if (result != VK_SUCCESS) { return framework->recordFailure(result, "vkQueueSubmit(SHARC clear)"); }
    return framework->waitRenderQueueIdle();
}

void RayTracingModule::onResourceReload() {
    if (clearSharcStorage() != VK_SUCCESS) { return; }
    sharcFrameIndex_ = 0;
    isFirstSharcFrame_ = true;
}

void RayTracingModule::updateSharcConfig(uint32_t frameIndex) {
    auto buffers = Renderer::instance().buffers();
    auto worldUbo = reinterpret_cast<vk::Data::WorldUBO *>(buffers->worldUniformBuffer()->mappedPtr());
    if (worldUbo == nullptr) { return; }

    glm::dvec3 currentCameraPos(worldUbo->cameraPos.x, worldUbo->cameraPos.y, worldUbo->cameraPos.z);
    if (isFirstSharcFrame_) {
        sharcPrevCameraPos_ = currentCameraPos;
        isFirstSharcFrame_ = false;
    }

    auto splitAddress = [](VkDeviceAddress address) {
        return std::array<uint32_t, 2>{static_cast<uint32_t>(address & 0xFFFFFFFFu),
                                       static_cast<uint32_t>((address >> 32) & 0xFFFFFFFFu)};
    };

    SharcConfigData config{};
    const auto hashEntriesAddress = splitAddress(sharcHashEntriesBuffer_->bufferAddress());
    const auto lockAddress = splitAddress(sharcLockBuffer_->bufferAddress());
    const auto accumulationAddress = splitAddress(sharcAccumulationBuffer_->bufferAddress());
    const auto resolvedAddress = splitAddress(sharcResolvedBuffer_->bufferAddress());
    config.hashEntriesAddress[0] = hashEntriesAddress[0];
    config.hashEntriesAddress[1] = hashEntriesAddress[1];
    config.lockAddress[0] = lockAddress[0];
    config.lockAddress[1] = lockAddress[1];
    config.accumulationAddress[0] = accumulationAddress[0];
    config.accumulationAddress[1] = accumulationAddress[1];
    config.resolvedAddress[0] = resolvedAddress[0];
    config.resolvedAddress[1] = resolvedAddress[1];
    config.cameraPosition = glm::vec4(static_cast<float>(currentCameraPos.x), static_cast<float>(currentCameraPos.y),
                                      static_cast<float>(currentCameraPos.z), 0.0f);
    config.cameraPositionPrev =
        glm::vec4(static_cast<float>(sharcPrevCameraPos_.x), static_cast<float>(sharcPrevCameraPos_.y),
                  static_cast<float>(sharcPrevCameraPos_.z), 0.0f);
    config.sceneScale = sharcSceneScale_;
    config.radianceScale = 1000.0f;
    config.accumulationFrameNum = sharcAccumulationFrameNum_;
    config.staleFrameNumMax = sharcStaleFrameNumMax_;
    config.capacity = sharcCapacity;
    config.frameIndex = sharcFrameIndex_;
    config.enableAntiFireflyFilter = 1;
    config.useLockBuffer = 0;
    config.debugMode = sharcDebugMode_;
    config.updateDownsampleFactor = sharcUpdateDownsampleFactor_;

    sharcConfigBuffers_[frameIndex]->uploadToBuffer(&config);

    sharcPrevCameraPos_ = currentCameraPos;
    sharcFrameIndex_++;
}

void RayTracingModule::initFullScreenPassTargets(FullScreenPass &pass,
                                                  std::shared_ptr<vk::Device> device,
                                                  uint32_t frameCount) {
    std::shared_ptr<vk::DeviceLocalImage> targetImage = findTargetImage(pass.config.target, 0);
    pass.renderPass = vk::RenderPassBuilder{}
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

    pass.framebuffers.resize(frameCount);
    for (uint32_t frameIndex = 0; frameIndex < frameCount; frameIndex++) {
        pass.framebuffers[frameIndex] =
            buildFramebuffers(findTargetImage(pass.config.target, frameIndex), pass.renderPass);
    }
}

std::vector<ShaderPack::ShaderCreateInfo>
RayTracingModule::collectFullScreenPassShaderRequests(
    const FullScreenPass &pass,
    const std::unordered_map<std::string, std::string> &definitions) {
    std::shared_ptr<vk::DeviceLocalImage> targetImage = findTargetImage(pass.config.target, 0);
    const bool implicitCubeFace =
        targetImage->layer() == 6 && !pass.config.face.has_value() && !pass.config.layer.has_value();
    const uint32_t pipelineCount = implicitCubeFace ? 6u : 1u;
    const uint32_t executionSet = shaderPack_->executionSet(5u);

    std::vector<ShaderPack::ShaderCreateInfo> requests(pipelineCount);
    for (uint32_t pipelineIndex = 0; pipelineIndex < pipelineCount; pipelineIndex++) {
        std::unordered_map<std::string, std::string> shaderDefinitions = definitions;
        if (targetImage->layer() == 6) {
            uint32_t faceIndex = pass.config.face.value_or(pipelineIndex);
            shaderDefinitions["FACE"] = std::to_string(faceIndex);
        }
        requests[pipelineIndex] = {
            .path = pass.config.fragmentShaderPath,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .definitions = std::move(shaderDefinitions),
            .executionStage = ShaderPackLoader::Stage::RayTracing,
            .executionSet = executionSet,
        };
    }
    return requests;
}

void RayTracingModule::buildFullScreenPassPipelines(
    FullScreenPass &pass,
    std::shared_ptr<vk::Device> device,
    const std::vector<std::shared_ptr<vk::Shader>> &compiledShaders,
    size_t &shaderOffset) {
    std::shared_ptr<vk::DeviceLocalImage> targetImage = findTargetImage(pass.config.target, 0);
    const bool implicitCubeFace =
        targetImage->layer() == 6 && !pass.config.face.has_value() && !pass.config.layer.has_value();
    const uint32_t pipelineCount = implicitCubeFace ? 6u : 1u;

    pass.fragmentShaders.assign(compiledShaders.begin() + static_cast<ptrdiff_t>(shaderOffset),
                                compiledShaders.begin() + static_cast<ptrdiff_t>(shaderOffset + pipelineCount));
    shaderOffset += pipelineCount;
    pass.pipelines.resize(pipelineCount);

    for (uint32_t pipelineIndex = 0; pipelineIndex < pipelineCount; pipelineIndex++) {
        pass.pipelines[pipelineIndex] =
            vk::GraphicsPipelineBuilder{}
                .defineRenderPass(pass.renderPass, 0)
                .beginShaderStage()
                .defineShaderStage(fullScreenVertexShader_, VK_SHADER_STAGE_VERTEX_BIT)
                .defineShaderStage(pass.fragmentShaders[pipelineIndex], VK_SHADER_STAGE_FRAGMENT_BIT)
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
                            .extent = {.width = targetImage->width(),
                                       .height = targetImage->height()},
                        },
                })
                .beginColorBlendAttachmentState()
                .defineDefaultColorBlendAttachmentState()
                .endColorBlendAttachmentState()
                .definePipelineLayout(rayTracingDescriptorTables_[0])
                .build(device);
    }
}

std::vector<ShaderPack::ShaderCreateInfo>
RayTracingModule::collectRayTracingPassShaderRequests(
    RayTracingPass &pass,
    const std::unordered_map<std::string, std::string> &definitions) {
    pass.missShaders.clear();
    pass.hitShaderGroups.clear();
    pass.hitGroupNameToIndex.clear();
    pass.shadowHitGroupIndex = 0;
    pass.fallbackHitGroupIndex = 0;
    pass.missGroupCount = 0;
    pass.hitGroupCount = 0;

    const uint32_t executionSet = shaderPack_->executionSet(5u);

    std::vector<ShaderPack::ShaderCreateInfo> requests;

    for (size_t i = 0; i < pass.config.missShaders.size(); i++) {
        requests.push_back({
            .path = pass.config.missShaders[i].shaderPath,
            .stage = VK_SHADER_STAGE_MISS_BIT_KHR,
            .definitions = definitions,
            .executionStage = ShaderPackLoader::Stage::RayTracing,
            .executionSet = executionSet,
        });
    }

    std::vector<RayTracingPass::HitAssignment> hitAssignments;

    pass.hitShaderGroups.resize(pass.config.hitGroups.size());
    for (size_t groupIndex = 0; groupIndex < pass.config.hitGroups.size(); groupIndex++) {
        const auto &groupConfig = pass.config.hitGroups[groupIndex];
        auto groupDefinitions = definitions;
        for (const auto &[key, value] : groupConfig.definitions) {
            groupDefinitions[key] = value;
        }
        HitShaderGroup group;
        group.name = groupConfig.name;
        group.type = groupConfig.type;
        if (groupConfig.closestHit.has_value()) {
            requests.push_back({
                .path = *groupConfig.closestHit,
                .stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                .definitions = groupDefinitions,
                .executionStage = ShaderPackLoader::Stage::RayTracing,
                .executionSet = executionSet,
            });
            hitAssignments.push_back({groupIndex, 0});
        }
        if (groupConfig.anyHit.has_value()) {
            requests.push_back({
                .path = *groupConfig.anyHit,
                .stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                .definitions = groupDefinitions,
                .executionStage = ShaderPackLoader::Stage::RayTracing,
                .executionSet = executionSet,
            });
            hitAssignments.push_back({groupIndex, 1});
        }
        if (groupConfig.intersection.has_value()) {
            requests.push_back({
                .path = *groupConfig.intersection,
                .stage = VK_SHADER_STAGE_INTERSECTION_BIT_KHR,
                .definitions = groupDefinitions,
                .executionStage = ShaderPackLoader::Stage::RayTracing,
                .executionSet = executionSet,
            });
            hitAssignments.push_back({groupIndex, 2});
        }

        pass.hitShaderGroups[groupIndex] = std::move(group);
        pass.hitGroupNameToIndex[pass.hitShaderGroups[groupIndex].name] = static_cast<uint32_t>(groupIndex);
        if (pass.hitShaderGroups[groupIndex].name == pass.config.defaultHitGroupName) {
            pass.fallbackHitGroupIndex = static_cast<uint32_t>(groupIndex);
        }
        if (pass.hitShaderGroups[groupIndex].name == "shadow") {
            pass.shadowHitGroupIndex = static_cast<uint32_t>(groupIndex);
        }
    }

    pass.missRequestCount_ = pass.config.missShaders.size();
    pass.hitRequestCount_ = hitAssignments.size();
    pass.hitAssignments_ = std::move(hitAssignments);

    std::unordered_map<std::string, std::string> queryDefinitions = definitions;
    if (pass.querySharcEnabled) {
        queryDefinitions["USE_SHARC"] = "1";
        queryDefinitions["SHARC_QUERY"] = "1";
    }

    std::unordered_map<std::string, std::string> updateDefinitions = definitions;
    if (pass.isSharcUpdatePass) {
        updateDefinitions["USE_SHARC"] = "1";
        updateDefinitions["SHARC_UPDATE"] = "1";
    }

    requests.push_back({
        .path = pass.config.rayGenShaderPath,
        .stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        .definitions = queryDefinitions,
        .executionStage = ShaderPackLoader::Stage::RayTracing,
        .executionSet = executionSet,
    });
    pass.rayGenRequestCount_ = 1;
    if (pass.isSharcUpdatePass) {
        requests.push_back({
            .path = pass.config.rayGenShaderPath,
            .stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            .definitions = updateDefinitions,
            .executionStage = ShaderPackLoader::Stage::RayTracing,
            .executionSet = executionSet,
        });
        pass.rayGenRequestCount_ = 2;
    }

    pass.sharcRequestCount_ = 0;
    if (pass.isSharcUpdatePass) {
        if (!shaderPack_->shaderPack().sharc.has_value()) {
            throw std::runtime_error("missing sharc config for update pass: " + pass.config.name);
        }
        requests.push_back({
            .path = shaderPack_->shaderPack().sharc->resolveCompShaderPath,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .definitions = updateDefinitions,
            .executionStage = ShaderPackLoader::Stage::RayTracing,
            .executionSet = executionSet,
        });
        pass.sharcRequestCount_ = 1;
    }

    return requests;
}

void RayTracingModule::buildRayTracingPassPipelines(
    RayTracingPass &pass,
    std::shared_ptr<vk::Device> device,
    const std::vector<std::shared_ptr<vk::Shader>> &compiledShaders,
    size_t &shaderOffset) {
    auto framework = framework_.lock();
    // This method runs on compiler workers without the recording thread's scene scope.
    // The owning module's descriptor slots are the authoritative, immutable view/frame layout.
    const uint32_t frameCount = static_cast<uint32_t>(rayTracingDescriptorTables_.size());

    pass.rayGenUpdateShader = nullptr;
    pass.rayGenQueryShader = nullptr;
    pass.updatePipeline = nullptr;
    pass.queryPipeline = nullptr;
    pass.updateSbts.clear();
    pass.querySbts.clear();
    pass.sharcResolveCompShader = nullptr;
    pass.sharcResolvePipeline = nullptr;

    for (size_t i = 0; i < pass.missRequestCount_; i++) {
        pass.missShaders.push_back({
            .name = pass.config.missShaders[i].name,
            .index = static_cast<uint32_t>(i),
            .shader = compiledShaders[shaderOffset + i],
        });
    }
    shaderOffset += pass.missRequestCount_;

    for (size_t i = 0; i < pass.hitRequestCount_; i++) {
        auto &group = pass.hitShaderGroups[pass.hitAssignments_[i].groupIndex];
        switch (pass.hitAssignments_[i].shaderType) {
            case 0: group.closestHitShader = compiledShaders[shaderOffset + i]; break;
            case 1: group.anyHitShader = compiledShaders[shaderOffset + i]; break;
            case 2: group.intersectionShader = compiledShaders[shaderOffset + i]; break;
        }
    }
    shaderOffset += pass.hitRequestCount_;
    if (pass.hitGroupNameToIndex.find("shadow") == pass.hitGroupNameToIndex.end()) {
        pass.shadowHitGroupIndex = pass.fallbackHitGroupIndex;
    }
    pass.hitGroupCount = static_cast<uint32_t>(pass.hitShaderGroups.size());
    pass.missGroupCount = static_cast<uint32_t>(pass.missShaders.size());

    auto buildRtPipeline = [&](const std::shared_ptr<vk::Shader> &rayGenShader) {
        vk::RayTracingPipelineBuilder builder;
        auto &stageBuilder = builder.beginShaderStage();

        uint32_t stageIndex = 0;
        stageBuilder.defineShaderStage(rayGenShader, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
        stageIndex++;

        std::vector<uint32_t> missStageIndices;
        for (const auto &missShader : pass.missShaders) {
            missStageIndices.push_back(stageIndex);
            stageBuilder.defineShaderStage(missShader.shader, VK_SHADER_STAGE_MISS_BIT_KHR);
            stageIndex++;
        }

        struct HitStageIndices {
            VkRayTracingShaderGroupTypeKHR type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
            uint32_t closestHit = VK_SHADER_UNUSED_KHR;
            uint32_t anyHit = VK_SHADER_UNUSED_KHR;
            uint32_t intersection = VK_SHADER_UNUSED_KHR;
        };
        std::vector<HitStageIndices> hitStageIndices;
        for (const auto &group : pass.hitShaderGroups) {
            HitStageIndices indices;
            indices.type = group.type;
            if (group.closestHitShader) {
                indices.closestHit = stageIndex;
                stageBuilder.defineShaderStage(group.closestHitShader, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
                stageIndex++;
            }
            if (group.anyHitShader) {
                indices.anyHit = stageIndex;
                stageBuilder.defineShaderStage(group.anyHitShader, VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
                stageIndex++;
            }
            if (group.intersectionShader) {
                indices.intersection = stageIndex;
                stageBuilder.defineShaderStage(group.intersectionShader, VK_SHADER_STAGE_INTERSECTION_BIT_KHR);
                stageIndex++;
            }
            hitStageIndices.push_back(indices);
        }
        stageBuilder.endShaderStage();

        auto &groupBuilder = builder.beginShaderGroup();
        groupBuilder.defineShaderGroup(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0, VK_SHADER_UNUSED_KHR,
                                       VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR);
        for (uint32_t missStageIndex : missStageIndices) {
            groupBuilder.defineShaderGroup(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, missStageIndex,
                                           VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR);
        }
        for (const auto &indices : hitStageIndices) {
            groupBuilder.defineShaderGroup(indices.type, VK_SHADER_UNUSED_KHR, indices.closestHit, indices.anyHit,
                                           indices.intersection);
        }
        groupBuilder.endShaderGroup();

        return builder.definePipelineLayout(rayTracingDescriptorTables_[0]).build(device);
    };

    pass.rayGenQueryShader = compiledShaders[shaderOffset];
    if (pass.isSharcUpdatePass && pass.rayGenRequestCount_ > 1) {
        pass.rayGenUpdateShader = compiledShaders[shaderOffset + 1];
    } else {
        pass.rayGenUpdateShader = pass.rayGenQueryShader;
    }
    shaderOffset += pass.rayGenRequestCount_;

    pass.queryPipeline = buildRtPipeline(pass.rayGenQueryShader);
    pass.updatePipeline = pass.isSharcUpdatePass ? buildRtPipeline(pass.rayGenUpdateShader) : pass.queryPipeline;

    if (pass.isSharcUpdatePass && pass.sharcRequestCount_ > 0) {
        pass.sharcResolveCompShader = compiledShaders[shaderOffset];
        shaderOffset += pass.sharcRequestCount_;
        pass.sharcResolvePipeline = vk::ComputePipelineBuilder{}
                                        .defineShader(pass.sharcResolveCompShader)
                                        .definePipelineLayout(rayTracingDescriptorTables_[0])
                                        .build(device);
    }

    pass.querySbts.resize(frameCount);
    if (pass.isSharcUpdatePass) { pass.updateSbts.resize(frameCount); }
    for (uint32_t frameIndex = 0; frameIndex < frameCount; frameIndex++) {
        if (pass.isSharcUpdatePass) {
            pass.updateSbts[frameIndex] = vk::SBT::create(framework->physicalDevice(), device, framework->vma(),
                                                          pass.updatePipeline, pass.missGroupCount, pass.hitGroupCount);
        }
        pass.querySbts[frameIndex] = vk::SBT::create(framework->physicalDevice(), device, framework->vma(),
                                                     pass.queryPipeline, pass.missGroupCount, pass.hitGroupCount);
    }
}

std::vector<ShaderPack::ShaderCreateInfo>
RayTracingModule::collectComputePassShaderRequests(
    const ComputePass &pass,
    const std::unordered_map<std::string, std::string> &definitions) {
    const uint32_t executionSet = shaderPack_->executionSet(5u);
    return {{
        .path = pass.config.computeShaderPath,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .definitions = definitions,
        .executionStage = ShaderPackLoader::Stage::RayTracing,
        .executionSet = executionSet,
    }};
}

void RayTracingModule::buildComputePassPipelines(
    ComputePass &pass,
    std::shared_ptr<vk::Device> device,
    const std::vector<std::shared_ptr<vk::Shader>> &compiledShaders,
    size_t &shaderOffset) {
    pass.computeShader = compiledShaders[shaderOffset++];
    pass.pipeline = vk::ComputePipelineBuilder{}
                        .defineShader(pass.computeShader)
                        .definePipelineLayout(rayTracingDescriptorTables_[0])
                        .build(device);
}

size_t RayTracingModule::shaderRequestCountForPass(PassVariant &passVariant) {
    return std::visit([&](auto &pass) -> size_t {
        using T = std::decay_t<decltype(pass)>;
        if constexpr (std::is_same_v<T, std::shared_ptr<FullScreenPass>>) {
            std::shared_ptr<vk::DeviceLocalImage> targetImage = findTargetImage(pass->config.target, 0);
            const bool implicitCubeFace =
                targetImage->layer() == 6 && !pass->config.face.has_value() && !pass->config.layer.has_value();
            return implicitCubeFace ? 6u : 1u;
        } else if constexpr (std::is_same_v<T, std::shared_ptr<RayTracingPass>>) {
            return pass->missRequestCount_ + pass->hitRequestCount_ + pass->rayGenRequestCount_ + pass->sharcRequestCount_;
        } else if constexpr (std::is_same_v<T, std::shared_ptr<ComputePass>>) {
            return 1u;
        }
        return 0u;
    }, passVariant);
}

void RayTracingModule::uploadStaticRayTracingPassSbts(std::shared_ptr<vk::Device> device) {
    auto framework = framework_.lock();
    if (framework == nullptr) { return; }

    bool hasStaticSbts = false;
    for (auto &passVariant : passes_) {
        bool passHasStaticSbts = std::visit([](auto &pass) {
            using T = std::decay_t<decltype(pass)>;
            if constexpr (std::is_same_v<T, std::shared_ptr<RayTracingPass>>) {
                return !pass->querySbts.empty() || !pass->updateSbts.empty();
            }
            return false;
        }, passVariant);
        if (passHasStaticSbts) {
            hasStaticSbts = true;
            break;
        }
    }
    if (!hasStaticSbts) { return; }

    auto commandPool = vk::CommandPool::create(framework->physicalDevice(), device);
    auto commandBuffer = vk::CommandBuffer::create(device, commandPool);
    commandBuffer->begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    for (auto &passVariant : passes_) {
        std::visit([&](auto &pass) {
            using T = std::decay_t<decltype(pass)>;
            if constexpr (std::is_same_v<T, std::shared_ptr<RayTracingPass>>) {
                for (auto &sbt : pass->updateSbts) {
                    if (sbt != nullptr) { sbt->uploadStaticSBT(commandBuffer); }
                }
                for (auto &sbt : pass->querySbts) {
                    if (sbt != nullptr) { sbt->uploadStaticSBT(commandBuffer); }
                }
            }
        }, passVariant);
    }
    const VkResult result = commandBuffer->end()->submitMainQueueIndividual(device);
    if (result != VK_SUCCESS) {
        framework->recordFailure(result, "vkQueueSubmit(SBT upload)");
        return;
    }
    if (framework->waitRenderQueueIdle() != VK_SUCCESS) { return; }
}

void RayTracingModule::renderFullScreenPass(
    const FullScreenPass &pass,
    RayTracingModuleContext &context) {
    mcvr::profile::Scope profile("pt.pass-fullscreen");
    auto frameworkContext = context.frameworkContext.lock();
    auto worldCommandBuffer = frameworkContext->worldCommandBuffer;
    uint32_t frameIndex = frameworkContext->frameIndex;
    auto framework = frameworkContext->framework.lock();

    std::vector<vk::CommandBuffer::BufferMemoryBarrier> bufferBarriers;
    std::vector<vk::CommandBuffer::ImageMemoryBarrier> imageBarriers;
    addPassResourceBarriers(pass.config.inputs.images, pass.config.inputs.buffers,
                            pass.config.outputs.images, pass.config.outputs.buffers,
                            bufferBarriers, imageBarriers, frameIndex,
                            framework->physicalDevice()->mainQueueIndex());
    addFullScreenTargetBarrier(pass.config.target, imageBarriers, frameIndex,
                               framework->physicalDevice()->mainQueueIndex());
    if (!bufferBarriers.empty() || !imageBarriers.empty()) {
        worldCommandBuffer->barriersBufferImage(bufferBarriers, imageBarriers);
    }

    std::shared_ptr<vk::DeviceLocalImage> targetImage = findTargetImage(pass.config.target, frameIndex);
    if (targetImage == nullptr) { return; }
    int faceCount = static_cast<int>(pass.framebuffers[frameIndex].size());
    int faceStart = 0;
    int faceEnd = faceCount;
    if (pass.config.face.has_value()) {
        faceStart = static_cast<int>(*pass.config.face);
        faceEnd = faceStart + 1;
    } else if (pass.config.layer.has_value()) {
        faceStart = static_cast<int>(*pass.config.layer);
        faceEnd = faceStart + 1;
    }
    for (int faceIndex = faceStart; faceIndex < faceEnd; faceIndex++) {
        size_t pipelineIndex = pass.pipelines.size() == 1 ? 0 : static_cast<size_t>(faceIndex);
        worldCommandBuffer->beginRenderPass({
            .renderPass = pass.renderPass,
            .framebuffer = pass.framebuffers[frameIndex][faceIndex],
            .renderAreaExtent = {targetImage->width(), targetImage->height()},
            .clearValues = {},
        });
        worldCommandBuffer->bindGraphicsPipeline(pass.pipelines[pipelineIndex])
            ->bindDescriptorTable(context.rayTracingDescriptorTable, VK_PIPELINE_BIND_POINT_GRAPHICS);
        worldCommandBuffer->draw(3, 1)->endRenderPass();
    }
}

void RayTracingModule::renderRayTracingPass(
    RayTracingPass &pass,
    RayTracingModuleContext &context,
    const std::unordered_map<std::string, ExecutionVariable> &variables) {
    mcvr::profile::Scope profile("pt.pass-raytrace");
    auto frameworkContext = context.frameworkContext.lock();
    auto framework = frameworkContext->framework.lock();
    auto worldCommandBuffer = frameworkContext->worldCommandBuffer;
    uint32_t frameIndex = frameworkContext->frameIndex;

    if (context.worldPrepareContext->tlas == nullptr) { return; }

    context.worldPrepareContext->setupHitGroupSbt(
        pass.hitGroupNameToIndex, pass.fallbackHitGroupIndex, pass.shadowHitGroupIndex, worldCommandBuffer,
        nullptr, pass.querySbts.at(frameIndex));

    std::vector<vk::CommandBuffer::BufferMemoryBarrier> bufferBarriers;
    std::vector<vk::CommandBuffer::ImageMemoryBarrier> imageBarriers;
    addPassResourceBarriers(pass.config.inputs.images, pass.config.inputs.buffers,
                            pass.config.outputs.images, pass.config.outputs.buffers,
                            bufferBarriers, imageBarriers, frameIndex,
                            framework->physicalDevice()->mainQueueIndex());
    if (!bufferBarriers.empty() || !imageBarriers.empty()) {
        worldCommandBuffer->barriersBufferImage(bufferBarriers, imageBarriers);
    }

    auto evaluateTraceDimension = [&](const std::string &expression, const std::string &axis) {
        double value = evaluateNumericExpression(expression, variables);
        if (!std::isfinite(value) || value <= 0.0) {
            throw std::runtime_error("invalid ray tracing dispatch " + axis + " for pass " +
                                     pass.config.name + ": " + expression);
        }
        return static_cast<uint32_t>(std::max(1.0, std::ceil(value)));
    };

    const uint32_t traceWidth = evaluateTraceDimension(pass.config.width, "width");
    const uint32_t traceHeight = evaluateTraceDimension(pass.config.height, "height");
    const uint32_t traceDepth = evaluateTraceDimension(pass.config.depth, "depth");

    if (pass.querySharcEnabled) {
        worldCommandBuffer->barriersMemory({{
            .srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        }});
    }

    worldCommandBuffer->bindDescriptorTable(context.rayTracingDescriptorTable, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
        ->bindRTPipeline(pass.queryPipeline)
        ->raytracing(pass.querySbts.at(frameIndex), traceWidth, traceHeight, traceDepth);
}

void RayTracingModule::renderSharcUpdateAndResolve(
    RayTracingPass &pass,
    RayTracingModuleContext &context,
    const ExecutionVariables &variables) {
    mcvr::profile::Scope profile("pt.pass-sharc");
    auto frameworkContext = context.frameworkContext.lock();
    auto framework = frameworkContext->framework.lock();
    auto worldCommandBuffer = frameworkContext->worldCommandBuffer;
    uint32_t frameIndex = frameworkContext->frameIndex;

    if (context.worldPrepareContext->tlas == nullptr) { return; }
    if (!pass.isSharcUpdatePass || pass.updatePipeline == nullptr || pass.sharcResolvePipeline == nullptr ||
        pass.updateSbts.empty()) {
        throw std::runtime_error("invalid sharc update pass state: " + pass.config.name);
    }

    context.worldPrepareContext->setupHitGroupSbt(
        pass.hitGroupNameToIndex, pass.fallbackHitGroupIndex, pass.shadowHitGroupIndex, worldCommandBuffer,
        pass.updateSbts.at(frameIndex), nullptr);

    updateSharcConfig(frameIndex);
    context.rayTracingDescriptorTable->bindBuffer(sharcConfigBuffers_[frameIndex], 4, 0);

    std::vector<vk::CommandBuffer::BufferMemoryBarrier> bufferBarriers;
    std::vector<vk::CommandBuffer::ImageMemoryBarrier> imageBarriers;
    addPassResourceBarriers(pass.config.inputs.images, pass.config.inputs.buffers,
                            pass.config.outputs.images, pass.config.outputs.buffers,
                            bufferBarriers, imageBarriers, frameIndex,
                            framework->physicalDevice()->mainQueueIndex());
    if (!bufferBarriers.empty() || !imageBarriers.empty()) {
        worldCommandBuffer->barriersBufferImage(bufferBarriers, imageBarriers);
    }

    auto evaluateTraceDimension = [&](const std::string &expression, const std::string &axis) {
        double value = evaluateNumericExpression(expression, variables);
        if (!std::isfinite(value) || value <= 0.0) {
            throw std::runtime_error("invalid ray tracing dispatch " + axis + " for pass " +
                                     pass.config.name + ": " + expression);
        }
        return static_cast<uint32_t>(std::max(1.0, std::ceil(value)));
    };

    const uint32_t traceWidth = evaluateTraceDimension(pass.config.width, "width");
    const uint32_t traceHeight = evaluateTraceDimension(pass.config.height, "height");
    const uint32_t updateDownsampleFactor = std::max(1u, sharcUpdateDownsampleFactor_);
    const uint32_t updateWidth = (traceWidth + updateDownsampleFactor - 1) / updateDownsampleFactor;
    const uint32_t updateHeight = (traceHeight + updateDownsampleFactor - 1) / updateDownsampleFactor;

    worldCommandBuffer->bindDescriptorTable(context.rayTracingDescriptorTable, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
        ->bindRTPipeline(pass.updatePipeline)
        ->raytracing(pass.updateSbts.at(frameIndex), updateWidth, updateHeight, 1);

    worldCommandBuffer->barriersMemory({{
        .srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
    }});

    worldCommandBuffer->bindDescriptorTable(context.rayTracingDescriptorTable, VK_PIPELINE_BIND_POINT_COMPUTE)
        ->bindComputePipeline(pass.sharcResolvePipeline);
    vkCmdDispatch(worldCommandBuffer->vkCommandBuffer(),
                  (sharcCapacity + sharcResolveWorkgroupSize - 1) / sharcResolveWorkgroupSize,
                  1, 1);
}

void RayTracingModule::renderComputePass(
    const ComputePass &pass,
    RayTracingModuleContext &context,
    const std::unordered_map<std::string, ExecutionVariable> &variables) {
    mcvr::profile::Scope profile("pt.pass-compute");
    auto frameworkContext = context.frameworkContext.lock();
    auto framework = frameworkContext->framework.lock();
    auto worldCommandBuffer = frameworkContext->worldCommandBuffer;
    uint32_t frameIndex = frameworkContext->frameIndex;

    std::vector<vk::CommandBuffer::BufferMemoryBarrier> bufferBarriers;
    std::vector<vk::CommandBuffer::ImageMemoryBarrier> imageBarriers;
    addPassResourceBarriers(pass.config.inputs.images, pass.config.inputs.buffers,
                            pass.config.outputs.images, pass.config.outputs.buffers,
                            bufferBarriers, imageBarriers, frameIndex,
                            framework->physicalDevice()->mainQueueIndex());
    if (!bufferBarriers.empty() || !imageBarriers.empty()) {
        worldCommandBuffer->barriersBufferImage(bufferBarriers, imageBarriers);
    }

    auto evaluateDispatchDimension = [&](const std::string &expression, const std::string &axis) {
        double value = evaluateNumericExpression(expression, variables);
        if (!std::isfinite(value) || value <= 0.0) {
            throw std::runtime_error("invalid compute dispatch " + axis + " for pass " +
                                     pass.config.name + ": " + expression);
        }
        return static_cast<uint32_t>(std::max(1.0, std::ceil(value)));
    };

    const uint32_t gx = evaluateDispatchDimension(pass.config.gx, "gx");
    const uint32_t gy = evaluateDispatchDimension(pass.config.gy, "gy");
    const uint32_t gz = evaluateDispatchDimension(pass.config.gz, "gz");

    worldCommandBuffer->bindDescriptorTable(context.rayTracingDescriptorTable, VK_PIPELINE_BIND_POINT_COMPUTE)
        ->bindComputePipeline(pass.pipeline);
    vkCmdDispatch(worldCommandBuffer->vkCommandBuffer(), gx, gy, gz);
}

void RayTracingModule::initPipelines() {
#ifdef DEBUG
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;
    auto tPhase = clock::now();
    auto printPhase = [&tPhase](const char *label) {
        auto now = clock::now();
        mcvr::log::debug("RayTracingModule") << "Initialize pipelines " << label << ": " << ms(now - tPhase).count() << " ms" << std::endl;
        tPhase = now;
    };
#endif

    auto framework = framework_.lock();
    auto device = framework->device();
    uint32_t frameCount = framework->recordingContextCount();
    const auto &shaderPack = shaderPack_->shaderPack();
    size_t executionBufferSize = shaderPack.rayTracingExecution.variables.size() * sizeof(float);
    // All passes bind one stable address. Inline updates carry each pass value in
    // queue order; changing the shared descriptor would retarget earlier draws.
    const auto stageExecutionBuffer =
        ShaderPack::createPassExecutionBuffer(device, framework->vma(), executionBufferSize);

    passes_.clear();
    passNameToPass_.clear();
    sharcUpdatePass_ = nullptr;

    for (const auto &passConfig : shaderPack.passes) {
        if (passConfig.stage != ShaderPackLoader::Stage::RayTracing) { continue; }
#ifdef DEBUG
        auto tPass = clock::now();
#endif
        switch (passConfig.type) {
            case ShaderPackLoader::PassConfig::Type::FullScreen: {
                std::shared_ptr<vk::DeviceLocalImage> targetImage = findTargetImage(passConfig.fullScreen.target, 0);
                if (targetImage == nullptr) {
                    throw std::runtime_error("Invalid full-screen pass target: " + passConfig.fullScreen.target);
                }
                if (passConfig.fullScreen.face.has_value() && targetImage->layer() != 6) {
                    throw std::runtime_error("Full-screen pass face requires cube target: " +
                                              passConfig.fullScreen.name);
                }
                if (passConfig.fullScreen.layer.has_value() && targetImage->layer() <= 1) {
                    throw std::runtime_error("Full-screen pass layer requires array target: " +
                                              passConfig.fullScreen.name);
                }
                if (passConfig.fullScreen.layer.has_value() && *passConfig.fullScreen.layer >= targetImage->layer()) {
                    throw std::runtime_error("Full-screen pass layer out of range: " +
                                              passConfig.fullScreen.name);
                }

                auto pass = std::make_shared<FullScreenPass>();
                pass->config = passConfig.fullScreen;
                pass->executionBuffer = stageExecutionBuffer;
                initFullScreenPassTargets(*pass, device, frameCount);

                passes_.push_back(pass);
                passNameToPass_.emplace(pass->config.name, pass);
                break;
            }
            case ShaderPackLoader::PassConfig::Type::Render:
                throw std::runtime_error("render pass is only supported by post_render stage: " +
                                          passConfig.render.name);
            case ShaderPackLoader::PassConfig::Type::RayTracing: {
                auto pass = std::make_shared<RayTracingPass>();
                pass->config = passConfig.rayTracing;
                pass->querySharcEnabled = hasSharcRuntime_ && passConfig.rayTracing.querySharc;
                pass->isSharcUpdatePass = false;
                pass->executionBuffer = stageExecutionBuffer;

                passes_.push_back(pass);
                passNameToPass_.emplace(pass->config.name, pass);
                break;
            }
            case ShaderPackLoader::PassConfig::Type::Compute: {
                auto pass = std::make_shared<ComputePass>();
                pass->config = passConfig.compute;
                pass->executionBuffer = stageExecutionBuffer;

                passes_.push_back(pass);
                passNameToPass_.emplace(pass->config.name, pass);
                break;
            }
        }
#ifdef DEBUG
        std::string dbgName;
        switch (passConfig.type) {
            case ShaderPackLoader::PassConfig::Type::FullScreen: dbgName = passConfig.fullScreen.name; break;
            case ShaderPackLoader::PassConfig::Type::RayTracing: dbgName = passConfig.rayTracing.name; break;
            case ShaderPackLoader::PassConfig::Type::Compute:    dbgName = passConfig.compute.name;    break;
            default: continue;
        }
        mcvr::log::debug("RayTracingModule") << "Create pass '" << dbgName << "': "
                  << ms(clock::now() - tPass).count() << " ms" << std::endl;
#endif
    }

    if (hasSharcRuntime_) {
        if (!shaderPack_->shaderPack().sharc.has_value()) {
            throw std::runtime_error("missing sharc config");
        }
        auto passIter = passNameToPass_.find(shaderPack_->shaderPack().sharc->updatePassName);
        if (passIter == passNameToPass_.end()) {
            throw std::runtime_error("unknown sharc update pass: " + shaderPack_->shaderPack().sharc->updatePassName);
        }
        auto updatePass = std::get_if<std::shared_ptr<RayTracingPass>>(&passIter->second);
        if (updatePass == nullptr || *updatePass == nullptr) {
            throw std::runtime_error("sharc update pass must be ray_tracing: " +
                                     shaderPack_->shaderPack().sharc->updatePassName);
        }
        sharcUpdatePass_ = *updatePass;
        sharcUpdatePass_->isSharcUpdatePass = true;
    }

#ifdef DEBUG
    printPhase("create all passes");
#endif

    std::vector<ShaderPack::ShaderCreateInfo> allShaderRequests;
    for (auto &passVariant : passes_) {
        std::visit([&](auto &pass) {
            using T = std::decay_t<decltype(pass)>;
            std::vector<ShaderPack::ShaderCreateInfo> requests;
            if constexpr (std::is_same_v<T, std::shared_ptr<FullScreenPass>>) {
                requests = collectFullScreenPassShaderRequests(*pass, pass->config.definitions);
            } else if constexpr (std::is_same_v<T, std::shared_ptr<RayTracingPass>>) {
                requests = collectRayTracingPassShaderRequests(*pass, pass->config.definitions);
            } else if constexpr (std::is_same_v<T, std::shared_ptr<ComputePass>>) {
                requests = collectComputePassShaderRequests(*pass, pass->config.definitions);
            }
            allShaderRequests.insert(allShaderRequests.end(), requests.begin(), requests.end());
        }, passVariant);
    }

#ifdef DEBUG
    printPhase("collect shader requests");

    ShaderPack::ShaderBatchStats shaderStats;
    std::vector<std::shared_ptr<vk::Shader>> allCompiledShaders =
        shaderPack_->createShaders(device, allShaderRequests, &shaderStats);

    std::string compileInfo = "build shader modules (" + std::to_string(shaderStats.requestCount) + " requests, " +
                              std::to_string(shaderStats.uniqueShaderCount) + " unique, " +
                              std::to_string(shaderStats.cacheHitCount) + " cache hits, " +
                              std::to_string(shaderStats.cacheMissCount) + " compiled";
    if (shaderStats.cacheReadFailureCount > 0) {
        compileInfo += ", " + std::to_string(shaderStats.cacheReadFailureCount) + " cache read failures";
    }
    compileInfo += ")";
    printPhase(compileInfo.c_str());
#else
    std::vector<std::shared_ptr<vk::Shader>> allCompiledShaders = shaderPack_->createShaders(device, allShaderRequests);
#endif

    std::vector<size_t> shaderOffsets(passes_.size());
    size_t totalShaderCount = 0;
    for (size_t i = 0; i < passes_.size(); ++i) {
        shaderOffsets[i] = totalShaderCount;
        totalShaderCount += shaderRequestCountForPass(passes_[i]);
    }
    if (totalShaderCount != allCompiledShaders.size()) {
        throw std::runtime_error("ray tracing shader offset count mismatch");
    }

    auto buildPassAtIndex = [&](size_t passIndex) {
        size_t shaderOffset = shaderOffsets[passIndex];
        std::visit([&](auto &pass) {
            using T = std::decay_t<decltype(pass)>;
            if constexpr (std::is_same_v<T, std::shared_ptr<FullScreenPass>>) {
                buildFullScreenPassPipelines(*pass, device, allCompiledShaders, shaderOffset);
            } else if constexpr (std::is_same_v<T, std::shared_ptr<RayTracingPass>>) {
                buildRayTracingPassPipelines(*pass, device, allCompiledShaders, shaderOffset);
            } else if constexpr (std::is_same_v<T, std::shared_ptr<ComputePass>>) {
                buildComputePassPipelines(*pass, device, allCompiledShaders, shaderOffset);
            }
        }, passes_[passIndex]);
    };

#ifdef DEBUG
    std::vector<std::string> passNames(passes_.size());
    std::vector<double> passBuildTimes(passes_.size(), 0.0);
    mcvr::parallelFor(passes_.size(), [&](size_t passIndex) {
        auto tPass = clock::now();
        std::visit([&](auto &pass) {
            passNames[passIndex] = pass->config.name;
        }, passes_[passIndex]);
        buildPassAtIndex(passIndex);
        passBuildTimes[passIndex] = ms(clock::now() - tPass).count();
    }, "MCVR_RT_PIPELINE_BUILD_THREADS");

    auto tSbtUpload = clock::now();
    uploadStaticRayTracingPassSbts(device);
    mcvr::log::debug("RayTracingModule") << "Upload static SBTs: "
              << ms(clock::now() - tSbtUpload).count() << " ms" << std::endl;

    for (size_t i = 0; i < passes_.size(); ++i) {
        mcvr::log::debug("RayTracingModule") << "Build pass '" << passNames[i] << "': "
                  << passBuildTimes[i] << " ms" << std::endl;
    }
    printPhase("build all passes");
#else
    mcvr::parallelFor(passes_.size(), [&](size_t passIndex) {
        buildPassAtIndex(passIndex);
    }, "MCVR_RT_PIPELINE_BUILD_THREADS");
    uploadStaticRayTracingPassSbts(device);
#endif
}

void RayTracingModule::initSBTs() {}

void RayTracingModule::initContexts() {
    auto framework = framework_.lock();
    auto worldPipeline = worldPipeline_.lock();
    uint32_t size = framework->recordingContextCount();
    contexts_.resize(size);

    for (uint32_t i = 0; i < size; i++) {
        contexts_[i] =
            RayTracingModuleContext::create(framework->contexts()[i], worldPipeline->contexts()[i], shared_from_this());
        worldPrepare_->contexts_[i]->rayTracingModuleContext =
            std::static_pointer_cast<RayTracingModuleContext>(contexts_[i]);
    }
}

void RayTracingModule::build() {
#ifdef DEBUG
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;
    auto t0 = clock::now();
    auto printStep = [&t0](const char *label) {
        auto now = clock::now();
        mcvr::log::debug("RayTracingModule") << "Build " << label << ": " << ms(now - t0).count() << " ms" << std::endl;
        t0 = now;
    };

    mcvr::log::debug("RayTracingModule") << "Build started" << std::endl;
#endif

    auto framework = framework_.lock();
#ifdef DEBUG
    printStep("lock framework");
#endif

    loadShaderPack();
#ifdef DEBUG
    printStep("loadShaderPack");
#endif

    initExecutionVariables();
#ifdef DEBUG
    printStep("initExecutionVariables");
#endif

    worldPrepare_->build();
#ifdef DEBUG
    printStep("worldPrepare build");
#endif

    initDescriptorTables();
#ifdef DEBUG
    printStep("initDescriptorTables");
#endif

    initRuntimeTextures();
#ifdef DEBUG
    printStep("initRuntimeTextures");
#endif

    initRuntimeBuffers();
#ifdef DEBUG
    printStep("initRuntimeBuffers");
#endif

    loadRuntimeResources();
#ifdef DEBUG
    printStep("loadRuntimeResources");
#endif

    initSharc();
#ifdef DEBUG
    printStep("initSharc");
#endif

    initPipelines();
#ifdef DEBUG
    printStep("initPipelines");
#endif

    initContexts();
#ifdef DEBUG
    printStep("initContexts");

    mcvr::log::debug("RayTracingModule") << "Build completed" << std::endl;
#endif
}

std::vector<std::shared_ptr<WorldModuleContext>> &RayTracingModule::contexts() {
    return contexts_;
}

void RayTracingModule::bindTexture(std::shared_ptr<vk::Sampler> sampler,
                                   std::shared_ptr<vk::DeviceLocalImage> image,
                                   int index) {
    textureBindings_.bind(index, {std::move(sampler), std::move(image)});
}

std::shared_ptr<vk::DescriptorTable> RayTracingModule::textureSnapshot() {
    return textureBindings_.acquire([&](const auto &bindings) {
        vk::DescriptorTableBuilder builder;
        builder.beginDescriptorLayoutSet().beginDescriptorLayoutSetBinding()
            .defineDescriptorLayoutSetBinding({0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096,
                VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_VERTEX_BIT |
                VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT})
            .endDescriptorLayoutSetBinding().endDescriptorLayoutSet();
        auto table = builder.build(framework_.lock()->device());
        for (const auto &[slot, binding] : bindings)
            table->bindSamplerImage(binding.first, binding.second, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    0, 0, slot);
        return table;
    });
}

void RayTracingModule::preClose() {
    contexts_.clear();
    passes_.clear();
    passNameToPass_.clear();
    sharcUpdatePass_ = nullptr;
    executionVariableConfigs_.clear();
    globalVariables_.clear();
    worldPrepare_ = nullptr;
    shaderPack_ = nullptr;
}

RayTracingModuleContext::RayTracingModuleContext(std::shared_ptr<FrameworkContext> frameworkContext,
                                                 std::shared_ptr<WorldPipelineContext> worldPipelineContext,
                                                 std::shared_ptr<RayTracingModule> rayTracingModule)
    : WorldModuleContext(frameworkContext, worldPipelineContext),
      rayTracingModule(rayTracingModule),
      rayTracingDescriptorTable(rayTracingModule->rayTracingDescriptorTables_[frameworkContext->frameIndex]),
      hdrNoisyOutputImage(rayTracingModule->hdrNoisyOutputImages_[frameworkContext->frameIndex]),
      diffuseAlbedoImage(rayTracingModule->diffuseAlbedoImages_[frameworkContext->frameIndex]),
      specularAlbedoImage(rayTracingModule->specularAlbedoImages_[frameworkContext->frameIndex]),
      normalRoughnessImage(rayTracingModule->normalRoughnessImages_[frameworkContext->frameIndex]),
      motionVectorImage(rayTracingModule->motionVectorImages_[frameworkContext->frameIndex]),
      linearDepthImage(rayTracingModule->linearDepthImages_[frameworkContext->frameIndex]),
      specularHitDepthImage(rayTracingModule->specularHitDepthImages_[frameworkContext->frameIndex]),
      firstHitDepthImage(rayTracingModule->firstHitDepthImages_[frameworkContext->frameIndex]),
      firstHitDiffuseDirectLightImage(
          rayTracingModule->firstHitDiffuseDirectLightImages_[frameworkContext->frameIndex]),
      firstHitDiffuseIndirectLightImage(
          rayTracingModule->firstHitDiffuseIndirectLightImages_[frameworkContext->frameIndex]),
      firstHitSpecularImage(rayTracingModule->firstHitSpecularImages_[frameworkContext->frameIndex]),
      firstHitClearImage(rayTracingModule->firstHitClearImages_[frameworkContext->frameIndex]),
      firstHitBaseEmissionImage(rayTracingModule->firstHitBaseEmissionImages_[frameworkContext->frameIndex]),
      fogImage(rayTracingModule->fogImages_[frameworkContext->frameIndex]),
      firstHitRefractionImage(rayTracingModule->firstHitRefractionImages_[frameworkContext->frameIndex]),
      worldPrepareContext(rayTracingModule->worldPrepare_->contexts_[frameworkContext->frameIndex]) {}

void RayTracingModuleContext::render() {
    mcvr::profile::Phases profile("pt.bind-frame-inputs");
    auto module = rayTracingModule.lock();
    if (module == nullptr) { return; }

    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto worldCommandBuffer = context->worldCommandBuffer;
    auto mainQueueIndex = framework->physicalDevice()->mainQueueIndex();

    auto buffers = Renderer::instance().buffers();
    auto chunks = Renderer::instance().world()->chunks();
    auto textureSnapshot = module->textureSnapshot();
    rayTracingDescriptorTable->useExternalSet(0, textureSnapshot, 0);
    framework->frameResourceRetainer().retain(textureSnapshot);
    module->refreshRuntimeBuffers(context->frameIndex);

    rayTracingDescriptorTable->bindBuffer(buffers->worldUniformBuffer(), 2, 0);
    rayTracingDescriptorTable->bindBuffer(buffers->lastWorldUniformBuffer(), 2, 1);
    rayTracingDescriptorTable->bindBuffer(buffers->skyUniformBuffer(), 2, 2);

    profile.next("pt.prepare-dispatch");
    mcvr::failure::runCheckedStage([&] { worldPrepareContext->render(); });
    profile.next("pt.bind-scene-inputs");
    if (Renderer::options.collectChunkEmission && chunks != nullptr && chunks->chunkPackedData() != nullptr) {
        rayTracingDescriptorTable->bindBuffer(chunks->chunkPackedData(), 1, 9);
    }
    if (worldPrepareContext->tlas != nullptr) {
        rayTracingDescriptorTable->bindAS(worldPrepareContext->tlas, 1, 0);
        rayTracingDescriptorTable->bindBuffer(worldPrepareContext->blasOffsetsBuffer, 1, 1);
        rayTracingDescriptorTable->bindBuffer(worldPrepareContext->indexBufferAddr, 1, 2);
        rayTracingDescriptorTable->bindBuffer(worldPrepareContext->lastIndexBufferAddr, 1, 3);
        rayTracingDescriptorTable->bindBuffer(worldPrepareContext->positionBufferAddr, 1, 4);
        rayTracingDescriptorTable->bindBuffer(worldPrepareContext->materialBufferAddr, 1, 5);
        rayTracingDescriptorTable->bindBuffer(worldPrepareContext->lastPositionBufferAddr, 1, 6);
        rayTracingDescriptorTable->bindBuffer(buffers->textureMappingBuffer(), 1, 7);
        rayTracingDescriptorTable->bindBuffer(worldPrepareContext->lastObjToWorldMat, 1, 8);
        rayTracingDescriptorTable->bindBuffer(worldPrepareContext->instanceAppearanceBuffer, 1, 10);
    }

    profile.next("pt.execution-variables");
    RayTracingModule::ExecutionVariables variables;
    variables.reserve(module->globalVariables_.size());
    for (const auto &[name, value] : module->globalVariables_) {
        auto config = module->findExecutionVariableConfig(name);
        variables.emplace(name,
                          RayTracingModule::ExecutionVariable{
                              .name = name,
                              .value = value,
                              .type = config.has_value() ? config->get().type : "",
                          });
    }

    profile.next("pt.sharc-dispatch");
    if (module->hasSharcRuntime_ && module->isSharcEnabled_ && module->sharcUpdatePass_ != nullptr) {
        auto &updateVariables = variables;
        if (module->sharcUpdatePass_->executionBuffer != nullptr) {
            module->shaderPack_->uploadExecutionBuffer(
                ShaderPackLoader::Stage::RayTracing, module->sharcUpdatePass_->executionBuffer, updateVariables,
                context->worldCommandBuffer, rayTracingDescriptorTable,
                module->shaderPack_->executionSet(5u), framework->physicalDevice()->mainQueueIndex(),
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        }
        module->renderSharcUpdateAndResolve(*module->sharcUpdatePass_, *this, updateVariables);
    }

    profile.next("pt.execution-program");
    auto executePass = [&](const std::string &passName, ShaderPack::ExecutionVariables &passVariables) {
        mcvr::profile::Scope passProfile("pt.pass-execute");
        mcvr::failure::throwIfFatal();
        auto passIter = module->passNameToPass_.find(passName);
        if (passIter == module->passNameToPass_.end()) { throw std::runtime_error("unknown pass: " + passName); }
        framework->device()->checkpoint(context->worldCommandBuffer->vkCommandBuffer(), passName);

        if (mcvr::diagnostics::device_loss::enabled())
            mcvr::diagnostics::device_loss::note("rt-pass-record", VK_SUCCESS,
                mcvr::diagnostics::device_loss::passFingerprint(passName), context->frameIndex);

        std::visit([&](auto &pass) {
            using T = std::decay_t<decltype(pass)>;

            if (pass->executionBuffer != nullptr) {
                module->shaderPack_->uploadExecutionBuffer(
                    ShaderPackLoader::Stage::RayTracing, pass->executionBuffer, passVariables,
                    context->worldCommandBuffer, rayTracingDescriptorTable,
                    module->shaderPack_->executionSet(5u), framework->physicalDevice()->mainQueueIndex(),
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
            }

            if constexpr (std::is_same_v<T, std::shared_ptr<FullScreenPass>>) {
                module->renderFullScreenPass(*pass, *this);
            } else if constexpr (std::is_same_v<T, std::shared_ptr<RayTracingPass>>) {
                module->renderRayTracingPass(*pass, *this, passVariables);
            } else if constexpr (std::is_same_v<T, std::shared_ptr<ComputePass>>) {
                module->renderComputePass(*pass, *this, passVariables);
            }
        }, passIter->second);
    };
    module->shaderPack_->executeCommands(
        ShaderPackLoader::Stage::RayTracing,
        module->shaderPack_->execution(ShaderPackLoader::Stage::RayTracing).commands, variables,
        module->executionExpressionVariables(), true, RayTracingModule::executionLoopLimit, executePass);

    profile.next("pt.execution-finish");
    for (auto &[name, value] : module->globalVariables_) {
        auto iter = variables.find(name);
        if (iter != variables.end()) { value = iter->second.value; }
    }
}
