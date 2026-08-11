#include "core/logging.hpp"
#include "nrd_module.hpp"
#include "core/render/buffers.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include <algorithm>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <limits>

nrd::ReblurSettings NrdModule::makeDefaultReblurSettings() {
    nrd::ReblurSettings settings = {};
    settings.maxAccumulatedFrameNum = 60;
    settings.maxFastAccumulatedFrameNum = 3;
    settings.fastHistoryClampingSigmaScale = 1.5f;
    settings.maxBlurRadius = 100.0f;
    settings.enableAntiFirefly = true;
    settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_5X5;
    return settings;
}

void NrdModule::updateReblurSettings() {
    auto &settings = reblurSettings_;
    settings.hitDistanceParameters.A = std::max(settings.hitDistanceParameters.A, 0.0f);
    settings.hitDistanceParameters.B = std::max(settings.hitDistanceParameters.B, 0.0f);
    settings.hitDistanceParameters.C = std::max(settings.hitDistanceParameters.C, 1.0f);

    settings.antilagSettings.luminanceSigmaScale = std::clamp(settings.antilagSettings.luminanceSigmaScale, 1.0f, 5.0f);
    settings.antilagSettings.luminanceSensitivity =
        std::clamp(settings.antilagSettings.luminanceSensitivity, 1.0f, 5.0f);

    settings.responsiveAccumulationSettings.roughnessThreshold =
        std::clamp(settings.responsiveAccumulationSettings.roughnessThreshold, 0.0f, 1.0f);
    settings.responsiveAccumulationSettings.minAccumulatedFrameNum =
        std::min(settings.responsiveAccumulationSettings.minAccumulatedFrameNum, 3u);

    settings.maxAccumulatedFrameNum = std::min(settings.maxAccumulatedFrameNum, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
    settings.maxFastAccumulatedFrameNum =
        std::min(settings.maxFastAccumulatedFrameNum, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
    settings.maxStabilizedFrameNum = std::min(settings.maxStabilizedFrameNum, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
    settings.historyFixFrameNum = std::min(settings.historyFixFrameNum, 3u);
    settings.historyFixBasePixelStride = std::max(settings.historyFixBasePixelStride, 1u);
    settings.historyFixAlternatePixelStride = std::max(settings.historyFixAlternatePixelStride, 1u);

    settings.fastHistoryClampingSigmaScale = std::clamp(settings.fastHistoryClampingSigmaScale, 1.0f, 3.0f);
    settings.diffusePrepassBlurRadius = std::max(settings.diffusePrepassBlurRadius, 0.0f);
    settings.specularPrepassBlurRadius = std::max(settings.specularPrepassBlurRadius, 0.0f);
    settings.minHitDistanceWeight = std::clamp(settings.minHitDistanceWeight, 0.0f, 0.2f);
    settings.minBlurRadius = std::max(settings.minBlurRadius, 0.0f);
    settings.maxBlurRadius = std::max(settings.maxBlurRadius, 0.0f);
    settings.lobeAngleFraction = std::clamp(settings.lobeAngleFraction, 0.0f, 1.0f);
    settings.roughnessFraction = std::clamp(settings.roughnessFraction, 0.0f, 1.0f);
    settings.planeDistanceSensitivity = std::clamp(settings.planeDistanceSensitivity, 0.0f, 1.0f);

    settings.specularProbabilityThresholdsForMvModification[0] =
        std::clamp(settings.specularProbabilityThresholdsForMvModification[0], 0.0f, 1.0f);
    settings.specularProbabilityThresholdsForMvModification[1] =
        std::clamp(settings.specularProbabilityThresholdsForMvModification[1], 0.0f, 1.0f);
    if (settings.specularProbabilityThresholdsForMvModification[0] >
        settings.specularProbabilityThresholdsForMvModification[1]) {
        std::swap(settings.specularProbabilityThresholdsForMvModification[0],
                  settings.specularProbabilityThresholdsForMvModification[1]);
    }

    settings.fireflySuppressorMinRelativeScale = std::clamp(settings.fireflySuppressorMinRelativeScale, 1.0f, 3.0f);
    settings.minMaterialForDiffuse = std::max(settings.minMaterialForDiffuse, 0.0f);
    settings.minMaterialForSpecular = std::max(settings.minMaterialForSpecular, 0.0f);
}

NrdModule::NrdModule() : reblurSettings_(makeDefaultReblurSettings()) {}

NrdModule::~NrdModule() {
    histories_.clear();
}

void NrdModule::init(std::shared_ptr<Framework> framework, std::shared_ptr<WorldPipeline> worldPipeline) {
    WorldModule::init(framework, worldPipeline);

    uint32_t size = framework->recordingContextCount();

    diffuseIndirectRadianceImages_.resize(size);
    specularIndirectRadianceImages_.resize(size);
    directRadianceImages_.resize(size);
    diffuseAlbedoMetallicImages_.resize(size);
    specularAlbedoImages_.resize(size);
    normalRoughnessImages_.resize(size);
    motionVectorImages_.resize(size);
    linearDepthImages_.resize(size);
    clearRadianceImages_.resize(size);
    baseEmissionImages_.resize(size);
    fogImages_.resize(size);
    diffuseHitDepthImages_.resize(size);
    specularHitDepthImages_.resize(size);
    refractionRadianceImages_.resize(size);
    denoisedRadianceImages_.resize(size);
    denoisedDiffuseRadianceImages_.resize(size);
    denoisedSpecularRadianceImages_.resize(size);
}

bool NrdModule::setOrCreateInputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                       std::vector<VkFormat> &formats,
                                       uint32_t frameIndex) {
    if (images.size() != inputImageNum) return false;

    auto framework = framework_.lock();
    auto createImage = [&](uint32_t index) {
        images[index] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, formats[index],
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    };

    for (uint32_t i = 0; i < images.size(); i++) {
        if (images[i] == nullptr) {
            if (width_ == 0 || height_ == 0) {
                mcvr::log::error("NrdModule") << "[NrdModule] Error: Cannot create input image " << i << " because dimensions are unknown."
                          << std::endl;
                return false;
            }
            createImage(i);
        }
    }

    diffuseIndirectRadianceImages_[frameIndex] = images[0];
    specularIndirectRadianceImages_[frameIndex] = images[1];
    directRadianceImages_[frameIndex] = images[2];
    diffuseAlbedoMetallicImages_[frameIndex] = images[3];
    specularAlbedoImages_[frameIndex] = images[4];
    normalRoughnessImages_[frameIndex] = images[5];
    motionVectorImages_[frameIndex] = images[6];
    linearDepthImages_[frameIndex] = images[7];
    clearRadianceImages_[frameIndex] = images[8];
    baseEmissionImages_[frameIndex] = images[9];
    fogImages_[frameIndex] = images[10];
    diffuseHitDepthImages_[frameIndex] = images[11];
    specularHitDepthImages_[frameIndex] = images[12];
    refractionRadianceImages_[frameIndex] = images[13];

    return true;
}

bool NrdModule::setOrCreateOutputImages(std::vector<std::shared_ptr<vk::DeviceLocalImage>> &images,
                                        std::vector<VkFormat> &formats,
                                        uint32_t frameIndex) {
    if (images.size() != outputImageNum) return false;
    if (images[0] == nullptr) return false;

    width_ = images[0]->width();
    height_ = images[0]->height();

    denoisedRadianceImages_[frameIndex] = images[0];

    return true;
}

void NrdModule::build() {
    auto framework = framework_.lock();
    auto worldPipeline = worldPipeline_.lock();
    uint32_t size = framework->recordingContextCount();

    updateReblurSettings();
    histories_.resize(worldPipeline->viewCount());
    for (auto &history : histories_) {
        history.wrapper_ = NrdWrapper::create(framework, width_, height_);
        history.wrapper_->setREBLURSettings(reblurSettings_);
    }

    initDescriptorTables();
    initImages();
    initPipeline();

    contexts_.resize(size);
    for (int i = 0; i < size; i++) {
        contexts_[i] =
            NrdModuleContext::create(framework->contexts()[i], worldPipeline->contexts()[i], shared_from_this());
    }
}

void NrdModule::setAttributes(int attributeCount, std::vector<std::string> &attributeKVs) {
    auto parseBool = [](const std::string &value) {
        return value == "render_pipeline.true" || value == "1" || value == "true" || value == "True" || value == "TRUE";
    };

    auto parseUint = [](const std::string &value, uint32_t fallback) {
        try {
            long long parsed = std::stoll(value);
            parsed = std::clamp(parsed, 0LL, static_cast<long long>(std::numeric_limits<uint32_t>::max()));
            return static_cast<uint32_t>(parsed);
        } catch (...) { return fallback; }
    };

    auto parseFloat = [](const std::string &value, float fallback) {
        try {
            return std::stof(value);
        } catch (...) { return fallback; }
    };

    auto parseCheckerboardMode = [](const std::string &value, nrd::CheckerboardMode fallback) {
        if (value == "off") { return nrd::CheckerboardMode::OFF; }
        if (value == "black") { return nrd::CheckerboardMode::BLACK; }
        if (value == "white") { return nrd::CheckerboardMode::WHITE; }
        return fallback;
    };

    auto parseHitDistanceReconstructionMode = [](const std::string &value,
                                                 nrd::HitDistanceReconstructionMode fallback) {
        if (value == "off") { return nrd::HitDistanceReconstructionMode::OFF; }
        if (value == "3x3") { return nrd::HitDistanceReconstructionMode::AREA_3X3; }
        if (value == "5x5") { return nrd::HitDistanceReconstructionMode::AREA_5X5; }
        return fallback;
    };

    for (int i = 0; i < attributeCount; i++) {
        const std::string &key = attributeKVs[2 * i];
        const std::string &value = attributeKVs[2 * i + 1];

        if (key == "render_pipeline.module.nrd.attribute.hit_distance_parameters_a") {
            reblurSettings_.hitDistanceParameters.A = parseFloat(value, reblurSettings_.hitDistanceParameters.A);
        } else if (key == "render_pipeline.module.nrd.attribute.hit_distance_parameters_b") {
            reblurSettings_.hitDistanceParameters.B = parseFloat(value, reblurSettings_.hitDistanceParameters.B);
        } else if (key == "render_pipeline.module.nrd.attribute.hit_distance_parameters_c") {
            reblurSettings_.hitDistanceParameters.C = parseFloat(value, reblurSettings_.hitDistanceParameters.C);
        } else if (key == "render_pipeline.module.nrd.attribute.antilag_luminance_sigma_scale") {
            reblurSettings_.antilagSettings.luminanceSigmaScale =
                parseFloat(value, reblurSettings_.antilagSettings.luminanceSigmaScale);
        } else if (key == "render_pipeline.module.nrd.attribute.antilag_luminance_sensitivity") {
            reblurSettings_.antilagSettings.luminanceSensitivity =
                parseFloat(value, reblurSettings_.antilagSettings.luminanceSensitivity);
        } else if (key == "render_pipeline.module.nrd.attribute.responsive_accumulation_roughness_threshold") {
            reblurSettings_.responsiveAccumulationSettings.roughnessThreshold =
                parseFloat(value, reblurSettings_.responsiveAccumulationSettings.roughnessThreshold);
        } else if (key == "render_pipeline.module.nrd.attribute.responsive_accumulation_min_accumulated_frame_num") {
            reblurSettings_.responsiveAccumulationSettings.minAccumulatedFrameNum =
                parseUint(value, reblurSettings_.responsiveAccumulationSettings.minAccumulatedFrameNum);
        } else if (key == "render_pipeline.module.nrd.attribute.max_accumulated_frame_num") {
            reblurSettings_.maxAccumulatedFrameNum = parseUint(value, reblurSettings_.maxAccumulatedFrameNum);
        } else if (key == "render_pipeline.module.nrd.attribute.max_fast_accumulated_frame_num") {
            reblurSettings_.maxFastAccumulatedFrameNum = parseUint(value, reblurSettings_.maxFastAccumulatedFrameNum);
        } else if (key == "render_pipeline.module.nrd.attribute.max_stabilized_frame_num") {
            reblurSettings_.maxStabilizedFrameNum = parseUint(value, reblurSettings_.maxStabilizedFrameNum);
        } else if (key == "render_pipeline.module.nrd.attribute.history_fix_frame_num") {
            reblurSettings_.historyFixFrameNum = parseUint(value, reblurSettings_.historyFixFrameNum);
        } else if (key == "render_pipeline.module.nrd.attribute.history_fix_base_pixel_stride") {
            reblurSettings_.historyFixBasePixelStride = parseUint(value, reblurSettings_.historyFixBasePixelStride);
        } else if (key == "render_pipeline.module.nrd.attribute.history_fix_alternate_pixel_stride") {
            reblurSettings_.historyFixAlternatePixelStride =
                parseUint(value, reblurSettings_.historyFixAlternatePixelStride);
        } else if (key == "render_pipeline.module.nrd.attribute.fast_history_clamping_sigma_scale") {
            reblurSettings_.fastHistoryClampingSigmaScale =
                parseFloat(value, reblurSettings_.fastHistoryClampingSigmaScale);
        } else if (key == "render_pipeline.module.nrd.attribute.diffuse_prepass_blur_radius") {
            reblurSettings_.diffusePrepassBlurRadius = parseFloat(value, reblurSettings_.diffusePrepassBlurRadius);
        } else if (key == "render_pipeline.module.nrd.attribute.specular_prepass_blur_radius") {
            reblurSettings_.specularPrepassBlurRadius = parseFloat(value, reblurSettings_.specularPrepassBlurRadius);
        } else if (key == "render_pipeline.module.nrd.attribute.min_hit_distance_weight") {
            reblurSettings_.minHitDistanceWeight = parseFloat(value, reblurSettings_.minHitDistanceWeight);
        } else if (key == "render_pipeline.module.nrd.attribute.min_blur_radius") {
            reblurSettings_.minBlurRadius = parseFloat(value, reblurSettings_.minBlurRadius);
        } else if (key == "render_pipeline.module.nrd.attribute.max_blur_radius") {
            reblurSettings_.maxBlurRadius = parseFloat(value, reblurSettings_.maxBlurRadius);
        } else if (key == "render_pipeline.module.nrd.attribute.lobe_angle_fraction") {
            reblurSettings_.lobeAngleFraction = parseFloat(value, reblurSettings_.lobeAngleFraction);
        } else if (key == "render_pipeline.module.nrd.attribute.roughness_fraction") {
            reblurSettings_.roughnessFraction = parseFloat(value, reblurSettings_.roughnessFraction);
        } else if (key == "render_pipeline.module.nrd.attribute.plane_distance_sensitivity") {
            reblurSettings_.planeDistanceSensitivity = parseFloat(value, reblurSettings_.planeDistanceSensitivity);
        } else if (key ==
                   "render_pipeline.module.nrd.attribute.specular_probability_thresholds_for_mv_modification_min") {
            reblurSettings_.specularProbabilityThresholdsForMvModification[0] =
                parseFloat(value, reblurSettings_.specularProbabilityThresholdsForMvModification[0]);
        } else if (key ==
                   "render_pipeline.module.nrd.attribute.specular_probability_thresholds_for_mv_modification_max") {
            reblurSettings_.specularProbabilityThresholdsForMvModification[1] =
                parseFloat(value, reblurSettings_.specularProbabilityThresholdsForMvModification[1]);
        } else if (key == "render_pipeline.module.nrd.attribute.firefly_suppressor_min_relative_scale") {
            reblurSettings_.fireflySuppressorMinRelativeScale =
                parseFloat(value, reblurSettings_.fireflySuppressorMinRelativeScale);
        } else if (key == "render_pipeline.module.nrd.attribute.min_material_for_diffuse") {
            reblurSettings_.minMaterialForDiffuse = parseFloat(value, reblurSettings_.minMaterialForDiffuse);
        } else if (key == "render_pipeline.module.nrd.attribute.min_material_for_specular") {
            reblurSettings_.minMaterialForSpecular = parseFloat(value, reblurSettings_.minMaterialForSpecular);
        } else if (key == "render_pipeline.module.nrd.attribute.checkerboard_mode") {
            reblurSettings_.checkerboardMode = parseCheckerboardMode(value, reblurSettings_.checkerboardMode);
        } else if (key == "render_pipeline.module.nrd.attribute.enable_anti_firefly") {
            reblurSettings_.enableAntiFirefly = parseBool(value);
        } else if (key == "render_pipeline.module.nrd.attribute.hit_distance_reconstruction_mode") {
            reblurSettings_.hitDistanceReconstructionMode =
                parseHitDistanceReconstructionMode(value, reblurSettings_.hitDistanceReconstructionMode);
        } else if (key == "render_pipeline.module.nrd.attribute.use_prepass_only_for_specular_motion_estimation") {
            reblurSettings_.usePrepassOnlyForSpecularMotionEstimation = parseBool(value);
        } else if (key == "render_pipeline.module.nrd.attribute.return_history_length_instead_of_occlusion") {
            reblurSettings_.returnHistoryLengthInsteadOfOcclusion = parseBool(value);
        }
    }

    updateReblurSettings();
}

std::vector<std::shared_ptr<WorldModuleContext>> &NrdModule::contexts() {
    return contexts_;
}

void NrdModule::bindTexture(std::shared_ptr<vk::Sampler> sampler,
                            std::shared_ptr<vk::DeviceLocalImage> image,
                            int index) {}

void NrdModule::onResourceReload() {
    for (auto &history : histories_) {
        history.nrdFrameIndex_ = 0;
        history.lastRefractionHistoryFrameIndex_ = -1;
    }
}

void NrdModule::preClose() {
    histories_.clear();
}

void NrdModule::initDescriptorTables() {
    auto framework = framework_.lock();
    uint32_t size = framework->recordingContextCount();

    composeDescriptorTables_.resize(size);
    prepareDescriptorTables_.resize(size);

    for (uint32_t i = 0; i < size; ++i) {
        {
            auto builder = vk::DescriptorTableBuilder{};
            auto &set0BindingBuilder = builder
                                           .beginDescriptorLayoutSet() // set 0
                                           .beginDescriptorLayoutSetBinding();

            for (int j = 0; j < 12; j++) {
                set0BindingBuilder.defineDescriptorLayoutSetBinding({
                    .binding = static_cast<uint32_t>(j),
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                });
            }

            auto &set1BindingBuilder = set0BindingBuilder.endDescriptorLayoutSetBinding()
                                           .endDescriptorLayoutSet()
                                           .beginDescriptorLayoutSet() // set 1
                                           .beginDescriptorLayoutSetBinding();

            for (int j = 0; j < 5; j++) {
                set1BindingBuilder.defineDescriptorLayoutSetBinding({
                    .binding = static_cast<uint32_t>(j),
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                });
            }

            auto &set2BindingBuilder = set1BindingBuilder.endDescriptorLayoutSetBinding()
                                           .endDescriptorLayoutSet()
                                           .beginDescriptorLayoutSet() // set 2
                                           .beginDescriptorLayoutSetBinding();

            set2BindingBuilder.defineDescriptorLayoutSetBinding({
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            });

            prepareDescriptorTables_[i] =
                set2BindingBuilder.endDescriptorLayoutSetBinding().endDescriptorLayoutSet().build(framework->device());
        }

        {
            auto builder = vk::DescriptorTableBuilder{};
            auto &set0BindingBuilder = builder
                                           .beginDescriptorLayoutSet() // set 0
                                           .beginDescriptorLayoutSetBinding();

            for (int j = 0; j < 9; j++) {
                set0BindingBuilder.defineDescriptorLayoutSetBinding({
                    .binding = static_cast<uint32_t>(j),
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                });
            }

            auto &set1BindingBuilder = set0BindingBuilder.endDescriptorLayoutSetBinding()
                                           .endDescriptorLayoutSet()
                                           .beginDescriptorLayoutSet() // set 1
                                           .beginDescriptorLayoutSetBinding();

            for (int j = 0; j < 7; j++) {
                set1BindingBuilder.defineDescriptorLayoutSetBinding({
                    .binding = static_cast<uint32_t>(j),
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                });
            }

            auto &set2BindingBuilder = set1BindingBuilder.endDescriptorLayoutSetBinding()
                                           .endDescriptorLayoutSet()
                                           .beginDescriptorLayoutSet() // set 2
                                           .beginDescriptorLayoutSetBinding();

            set2BindingBuilder.defineDescriptorLayoutSetBinding({
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            });
            set2BindingBuilder.defineDescriptorLayoutSetBinding({
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            });

            composeDescriptorTables_[i] =
                set2BindingBuilder.endDescriptorLayoutSetBinding().endDescriptorLayoutSet().build(framework->device());
        }
    }
}

void NrdModule::initImages() {
    auto framework = framework_.lock();
    uint32_t size = framework->recordingContextCount();

    nrdDiffuseRadianceImages_.resize(size);
    nrdSpecularRadianceImages_.resize(size);
    nrdMotionVectorImages_.resize(size);
    nrdNormalRoughnessImages_.resize(size);
    nrdLinearDepthImages_.resize(size);
    denoisedDiffuseRadianceImages_.resize(size);
    denoisedSpecularRadianceImages_.resize(size);
    refractionHistoryRadianceImages_.resize(size);
    refractionHistoryDepthImages_.resize(size);
    userTexturePools_.resize(size);

    for (uint32_t i = 0; i < size; i++) {
        refractionHistoryRadianceImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        refractionHistoryDepthImages_[i] =
            vk::DeviceLocalImage::create(framework->device(), framework->vma(), false, width_, height_, 1,
                                         VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }

    for (uint32_t i = 0; i < size; i++) {
        userTexturePools_[i] =
            std::make_shared<std::array<std::shared_ptr<vk::DeviceLocalImage>, size_t(nrd::ResourceType::MAX_NUM)>>();

        prepareDescriptorTables_[i]->bindImage(directRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 0);
        prepareDescriptorTables_[i]->bindImage(diffuseIndirectRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        prepareDescriptorTables_[i]->bindImage(specularIndirectRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 2);
        prepareDescriptorTables_[i]->bindImage(diffuseAlbedoMetallicImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 3);
        prepareDescriptorTables_[i]->bindImage(specularAlbedoImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 4);
        prepareDescriptorTables_[i]->bindImage(motionVectorImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 5);
        prepareDescriptorTables_[i]->bindImage(normalRoughnessImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 6);
        prepareDescriptorTables_[i]->bindImage(linearDepthImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 7);
        prepareDescriptorTables_[i]->bindImage(clearRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 8);
        prepareDescriptorTables_[i]->bindImage(diffuseHitDepthImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 9);
        prepareDescriptorTables_[i]->bindImage(specularHitDepthImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 10);
        prepareDescriptorTables_[i]->bindImage(refractionRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 11);

        nrdDiffuseRadianceImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        prepareDescriptorTables_[i]->bindImage(nrdDiffuseRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 1, 0);
        userTexturePools_[i]->at((size_t)nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST) = nrdDiffuseRadianceImages_[i];

        nrdSpecularRadianceImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        prepareDescriptorTables_[i]->bindImage(nrdSpecularRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 1, 1);
        userTexturePools_[i]->at((size_t)nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST) = nrdSpecularRadianceImages_[i];

        nrdMotionVectorImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, VK_FORMAT_R16G16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        prepareDescriptorTables_[i]->bindImage(nrdMotionVectorImages_[i], VK_IMAGE_LAYOUT_GENERAL, 1, 2);
        userTexturePools_[i]->at((size_t)nrd::ResourceType::IN_MV) = nrdMotionVectorImages_[i];

        nrdNormalRoughnessImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, NrdWrapper::getNormalRoughnessFormat(),
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        prepareDescriptorTables_[i]->bindImage(nrdNormalRoughnessImages_[i], VK_IMAGE_LAYOUT_GENERAL, 1, 3);
        userTexturePools_[i]->at((size_t)nrd::ResourceType::IN_NORMAL_ROUGHNESS) = nrdNormalRoughnessImages_[i];

        nrdLinearDepthImages_[i] =
            vk::DeviceLocalImage::create(framework->device(), framework->vma(), false, width_, height_, 1,
                                         VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        prepareDescriptorTables_[i]->bindImage(nrdLinearDepthImages_[i], VK_IMAGE_LAYOUT_GENERAL, 1, 4);
        userTexturePools_[i]->at((size_t)nrd::ResourceType::IN_VIEWZ) = nrdLinearDepthImages_[i];

        composeDescriptorTables_[i]->bindImage(diffuseAlbedoMetallicImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 0);
        composeDescriptorTables_[i]->bindImage(specularAlbedoImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        composeDescriptorTables_[i]->bindImage(normalRoughnessImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 2);
        composeDescriptorTables_[i]->bindImage(linearDepthImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 3);
        composeDescriptorTables_[i]->bindImage(clearRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 4);
        composeDescriptorTables_[i]->bindImage(baseEmissionImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 5);
        composeDescriptorTables_[i]->bindImage(refractionRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 6);
        composeDescriptorTables_[i]->bindImage(motionVectorImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 7);
        composeDescriptorTables_[i]->bindImage(fogImages_[i], VK_IMAGE_LAYOUT_GENERAL, 0, 8);

        denoisedDiffuseRadianceImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        composeDescriptorTables_[i]->bindImage(denoisedDiffuseRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 1, 0);
        userTexturePools_[i]->at((size_t)nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST) =
            denoisedDiffuseRadianceImages_[i];

        denoisedSpecularRadianceImages_[i] = vk::DeviceLocalImage::create(
            framework->device(), framework->vma(), false, width_, height_, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        composeDescriptorTables_[i]->bindImage(denoisedSpecularRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 1, 1);
        userTexturePools_[i]->at((size_t)nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST) =
            denoisedSpecularRadianceImages_[i];

        composeDescriptorTables_[i]->bindImage(denoisedRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 1, 2);

        uint32_t fallbackPrevIndex = (i + 1) % size;
        composeDescriptorTables_[i]->bindImage(refractionHistoryRadianceImages_[fallbackPrevIndex],
                                               VK_IMAGE_LAYOUT_GENERAL, 1, 3);
        composeDescriptorTables_[i]->bindImage(refractionHistoryDepthImages_[fallbackPrevIndex],
                                               VK_IMAGE_LAYOUT_GENERAL, 1, 4);
        composeDescriptorTables_[i]->bindImage(refractionHistoryRadianceImages_[i], VK_IMAGE_LAYOUT_GENERAL, 1, 5);
        composeDescriptorTables_[i]->bindImage(refractionHistoryDepthImages_[i], VK_IMAGE_LAYOUT_GENERAL, 1, 6);
    }

    auto commandPool = vk::CommandPool::create(framework->physicalDevice(), framework->device());
    auto commandBuffer = vk::CommandBuffer::create(framework->device(), commandPool);
    commandBuffer->begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    for (uint32_t i = 0; i < size; i++) {
        auto clearImage = [&](const std::shared_ptr<vk::DeviceLocalImage> &image, const VkClearColorValue &clearValue) {
            commandBuffer->barriersBufferImage({},
                                               {{
                                                   .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                                   .srcAccessMask = 0,
                                                   .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                                   .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                   .oldLayout = image->imageLayout(),
                                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                   .srcQueueFamilyIndex = framework->physicalDevice()->mainQueueIndex(),
                                                   .dstQueueFamilyIndex = framework->physicalDevice()->mainQueueIndex(),
                                                   .image = image,
                                                   .subresourceRange = vk::wholeColorSubresourceRange,
                                               }});
            image->imageLayout() = VK_IMAGE_LAYOUT_GENERAL;
            vkCmdClearColorImage(commandBuffer->vkCommandBuffer(), image->vkImage(), VK_IMAGE_LAYOUT_GENERAL,
                                 &clearValue, 1, &vk::wholeColorSubresourceRange);
        };

        const VkClearColorValue clearRefraction = {{0.0f, 0.0f, 0.0f, 0.0f}};
        const VkClearColorValue clearDepth = {{65504.0f, 0.0f, 0.0f, 0.0f}};
        clearImage(refractionHistoryRadianceImages_[i], clearRefraction);
        clearImage(refractionHistoryDepthImages_[i], clearDepth);
    }

    commandBuffer->end();
    VkResult result = commandBuffer->submitMainQueueIndividual(framework->device());
    if (result != VK_SUCCESS) {
        framework->recordFailure(result, "vkQueueSubmit(NRD history clear)");
        return;
    }
    if (framework->waitRenderQueueIdle() != VK_SUCCESS) { return; }
}

void NrdModule::initPipeline() {
    auto framework = framework_.lock();
    auto device = framework->device();
    std::filesystem::path shaderPath = Renderer::folderPath / "shaders";

    auto prepareShader = vk::Shader::create(device, (shaderPath / "world/nrd/prepare_comp.spv").string());
    preparePipeline_ = vk::ComputePipelineBuilder{}
                           .defineShader(prepareShader)
                           .definePipelineLayout(prepareDescriptorTables_[0])
                           .build(device);

    auto composeShader = vk::Shader::create(device, (shaderPath / "world/nrd/compose_comp.spv").string());
    composePipeline_ = vk::ComputePipelineBuilder{}
                           .defineShader(composeShader)
                           .definePipelineLayout(composeDescriptorTables_[0])
                           .build(device);
}

NrdModuleContext::NrdModuleContext(std::shared_ptr<FrameworkContext> frameworkContext,
                                   std::shared_ptr<WorldPipelineContext> worldPipelineContext,
                                   std::shared_ptr<NrdModule> nrdModule)
    : WorldModuleContext(frameworkContext, worldPipelineContext),
      nrdModule(nrdModule),
      diffuseIndirectRadianceImage(nrdModule->diffuseIndirectRadianceImages_[frameworkContext->frameIndex]),
      specularIndirectRadianceImage(nrdModule->specularIndirectRadianceImages_[frameworkContext->frameIndex]),
      directRadianceImage(nrdModule->directRadianceImages_[frameworkContext->frameIndex]),
      diffuseAlbedoMetallicImage(nrdModule->diffuseAlbedoMetallicImages_[frameworkContext->frameIndex]),
      specularAlbedoImage(nrdModule->specularAlbedoImages_[frameworkContext->frameIndex]),
      normalRoughnessImage(nrdModule->normalRoughnessImages_[frameworkContext->frameIndex]),
      motionVectorImage(nrdModule->motionVectorImages_[frameworkContext->frameIndex]),
      linearDepthImage(nrdModule->linearDepthImages_[frameworkContext->frameIndex]),
      clearRadianceImage(nrdModule->clearRadianceImages_[frameworkContext->frameIndex]),
      baseEmissionImage(nrdModule->baseEmissionImages_[frameworkContext->frameIndex]),
      fogImage(nrdModule->fogImages_[frameworkContext->frameIndex]),
      diffuseHitDepthImage(nrdModule->diffuseHitDepthImages_[frameworkContext->frameIndex]),
      specularHitDepthImage(nrdModule->specularHitDepthImages_[frameworkContext->frameIndex]),
      refractionRadianceImage(nrdModule->refractionRadianceImages_[frameworkContext->frameIndex]),
      nrdDiffuseRadianceImage(nrdModule->nrdDiffuseRadianceImages_[frameworkContext->frameIndex]),
      nrdSpecularRadianceImage(nrdModule->nrdSpecularRadianceImages_[frameworkContext->frameIndex]),
      nrdMotionVectorImage(nrdModule->nrdMotionVectorImages_[frameworkContext->frameIndex]),
      nrdNormalRoughnessImage(nrdModule->nrdNormalRoughnessImages_[frameworkContext->frameIndex]),
      nrdLinearDepthImage(nrdModule->nrdLinearDepthImages_[frameworkContext->frameIndex]),
      denoisedDiffuseRadianceImage(nrdModule->denoisedDiffuseRadianceImages_[frameworkContext->frameIndex]),
      denoisedSpecularRadianceImage(nrdModule->denoisedSpecularRadianceImages_[frameworkContext->frameIndex]),
      refractionHistoryRadianceImage(nrdModule->refractionHistoryRadianceImages_[frameworkContext->frameIndex]),
      refractionHistoryDepthImage(nrdModule->refractionHistoryDepthImages_[frameworkContext->frameIndex]),
      prepareDescriptorTable(nrdModule->prepareDescriptorTables_[frameworkContext->frameIndex]),
      composeDescriptorTable(nrdModule->composeDescriptorTables_[frameworkContext->frameIndex]),
      userTexturePool(nrdModule->userTexturePools_[frameworkContext->frameIndex]),
      denoisedRadianceImage(nrdModule->denoisedRadianceImages_[frameworkContext->frameIndex]) {}

void NrdModuleContext::render() {
    auto module = nrdModule.lock();
    if (!module || !module->histories_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).wrapper_) return;

    auto context = frameworkContext.lock();
    auto framework = context->framework.lock();
    auto worldCommandBuffer = context->worldCommandBuffer;
    auto buffers = Renderer::instance().buffers();

    auto worldUBOBuffer = buffers->worldUniformBuffer();
    auto worldUBO = static_cast<vk::Data::WorldUBO *>(worldUBOBuffer->mappedPtr());
    auto skyUBOBuffer = buffers->skyUniformBuffer();

    auto lastWorldUBOBuffer = buffers->lastWorldUniformBuffer();
    auto lastWorldUBO = static_cast<vk::Data::WorldUBO *>(lastWorldUBOBuffer->mappedPtr());

    auto mainQueueIndex = framework->physicalDevice()->mainQueueIndex();

    auto addBarrier = [mainQueueIndex](std::vector<vk::CommandBuffer::ImageMemoryBarrier> &barriers,
                                       std::shared_ptr<vk::DeviceLocalImage> &image, VkImageLayout targetLayout) {
        VkPipelineStageFlags2 srcStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        VkAccessFlags2 srcAccess = 0;
        if (image->imageLayout() == VK_IMAGE_LAYOUT_GENERAL) {
            srcStage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            srcAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        } else if (image->imageLayout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            srcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            srcAccess = VK_ACCESS_2_SHADER_READ_BIT;
        }
        barriers.push_back({
            .srcStageMask = srcStage,
            .srcAccessMask = srcAccess,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            .oldLayout = image->imageLayout(),
            .newLayout = targetLayout,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .image = image,
            .subresourceRange = vk::wholeColorSubresourceRange,
        });
        image->imageLayout() = targetLayout;
    };

    // config nrd
    {
        nrd::CommonSettings commonSettings = {};
        glm::mat4 nrdViewToClip = worldUBO->cameraProjMat;
        glm::mat4 nrdViewToClipPrev = lastWorldUBO->cameraProjMat;
        for (int column = 0; column < 4; ++column) {
            nrdViewToClip[column][1] *= -1.0f;
            nrdViewToClipPrev[column][1] *= -1.0f;
        }

        std::memcpy(commonSettings.viewToClipMatrix, glm::value_ptr(nrdViewToClip), sizeof(glm::mat4));
        std::memcpy(commonSettings.viewToClipMatrixPrev, glm::value_ptr(nrdViewToClipPrev),
                    sizeof(glm::mat4));
        std::memcpy(commonSettings.worldToViewMatrix, glm::value_ptr(worldUBO->cameraEffectedViewMat),
                    sizeof(glm::mat4));
        std::memcpy(commonSettings.worldToViewMatrixPrev, glm::value_ptr(lastWorldUBO->cameraEffectedViewMat),
                    sizeof(glm::mat4));

        commonSettings.resourceSize[0] = static_cast<uint16_t>(module->width_);
        commonSettings.resourceSize[1] = static_cast<uint16_t>(module->height_);
        commonSettings.rectSize[0] = static_cast<uint16_t>(module->width_);
        commonSettings.rectSize[1] = static_cast<uint16_t>(module->height_);
        commonSettings.resourceSizePrev[0] = static_cast<uint16_t>(module->width_);
        commonSettings.resourceSizePrev[1] = static_cast<uint16_t>(module->height_);
        commonSettings.rectSizePrev[0] = static_cast<uint16_t>(module->width_);
        commonSettings.rectSizePrev[1] = static_cast<uint16_t>(module->height_);

        commonSettings.cameraJitter[0] = worldUBO->cameraJitter.x;
        commonSettings.cameraJitter[1] = worldUBO->cameraJitter.y;
        commonSettings.cameraJitterPrev[0] = lastWorldUBO->cameraJitter.x;
        commonSettings.cameraJitterPrev[1] = lastWorldUBO->cameraJitter.y;
        commonSettings.frameIndex = module->histories_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).nrdFrameIndex_;
        commonSettings.accumulationMode =
            module->histories_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).nrdFrameIndex_ == 0 ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;

        commonSettings.motionVectorScale[0] = 1.0f / module->width_;
        commonSettings.motionVectorScale[1] = 1.0f / module->height_;
        commonSettings.motionVectorScale[2] = 0.0f;
        commonSettings.viewZScale = 1.0f;
        commonSettings.disocclusionThreshold = 0.02f;
        commonSettings.disocclusionThresholdAlternate = 0.15f;
        commonSettings.isMotionVectorInWorldSpace = false;

        commonSettings.isDisocclusionThresholdMixAvailable = false;
        commonSettings.enableValidation = false;

        module->histories_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).wrapper_->setCommonSettings(commonSettings);
        module->histories_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).wrapper_->setUserPoolTexture(userTexturePool);
        module->histories_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).nrdFrameIndex_++;
    }

    // prepare
    {
        prepareDescriptorTable->bindBuffer(worldUBOBuffer, 2, 0);

        std::vector<vk::CommandBuffer::ImageMemoryBarrier> imageBarriers;
        addBarrier(imageBarriers, directRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, diffuseIndirectRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, specularIndirectRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, diffuseAlbedoMetallicImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, specularAlbedoImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, motionVectorImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, normalRoughnessImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, linearDepthImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, clearRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, diffuseHitDepthImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, specularHitDepthImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, refractionRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, nrdDiffuseRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, nrdSpecularRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, nrdMotionVectorImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, nrdNormalRoughnessImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, nrdLinearDepthImage, VK_IMAGE_LAYOUT_GENERAL);

        worldCommandBuffer->barriersBufferImage({}, imageBarriers);

        worldCommandBuffer->bindDescriptorTable(prepareDescriptorTable, VK_PIPELINE_BIND_POINT_COMPUTE)
            ->bindComputePipeline(module->preparePipeline_);
        vkCmdDispatch(worldCommandBuffer->vkCommandBuffer(), (module->width_ + 15) / 16, (module->height_ + 15) / 16,
                      1);
    }

    // denoise
    {
        std::vector<vk::CommandBuffer::ImageMemoryBarrier> imageBarriers;
        addBarrier(imageBarriers, nrdDiffuseRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, nrdSpecularRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, nrdMotionVectorImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, nrdNormalRoughnessImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, nrdLinearDepthImage, VK_IMAGE_LAYOUT_GENERAL);

        worldCommandBuffer->barriersBufferImage({}, imageBarriers);

        nrd::Identifier denoiser = nrd::Identifier(nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR);
        module->histories_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).wrapper_->denoise(&denoiser, 1, worldCommandBuffer->vkCommandBuffer());
    }

    // composition
    {
        const uint32_t historyImageCount = module->worldPipeline_.lock()->framesPerView();
        const uint32_t viewBase = (context->frameIndex / historyImageCount) * historyImageCount;
        const uint32_t currentFrameIndex = context->frameIndex;
        uint32_t prevFrameIndex = viewBase + (currentFrameIndex + historyImageCount - 1) % historyImageCount;
        if (module->histories_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).lastRefractionHistoryFrameIndex_ >= 0) {
            prevFrameIndex = static_cast<uint32_t>(module->histories_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).lastRefractionHistoryFrameIndex_);
        }
        if (prevFrameIndex == currentFrameIndex && historyImageCount > 1) {
            prevFrameIndex = viewBase + (currentFrameIndex + 1) % historyImageCount;
        }

        auto refractionHistoryRadianceImagePrev = module->refractionHistoryRadianceImages_[prevFrameIndex];
        auto refractionHistoryDepthImagePrev = module->refractionHistoryDepthImages_[prevFrameIndex];

        composeDescriptorTable->bindImage(refractionHistoryRadianceImagePrev, VK_IMAGE_LAYOUT_GENERAL, 1, 3);
        composeDescriptorTable->bindImage(refractionHistoryDepthImagePrev, VK_IMAGE_LAYOUT_GENERAL, 1, 4);
        composeDescriptorTable->bindImage(refractionHistoryRadianceImage, VK_IMAGE_LAYOUT_GENERAL, 1, 5);
        composeDescriptorTable->bindImage(refractionHistoryDepthImage, VK_IMAGE_LAYOUT_GENERAL, 1, 6);

        std::vector<vk::CommandBuffer::ImageMemoryBarrier> imageBarriers;
        addBarrier(imageBarriers, diffuseAlbedoMetallicImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, specularAlbedoImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, normalRoughnessImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, linearDepthImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, clearRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, baseEmissionImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, fogImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, refractionRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, motionVectorImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, denoisedDiffuseRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, denoisedSpecularRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, refractionHistoryRadianceImagePrev, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, refractionHistoryDepthImagePrev, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, refractionHistoryRadianceImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, refractionHistoryDepthImage, VK_IMAGE_LAYOUT_GENERAL);
        addBarrier(imageBarriers, denoisedRadianceImage, VK_IMAGE_LAYOUT_GENERAL);

        worldCommandBuffer->barriersBufferImage({}, imageBarriers);

        composeDescriptorTable->bindBuffer(worldUBOBuffer, 2, 0);
        composeDescriptorTable->bindBuffer(skyUBOBuffer, 2, 1);

        worldCommandBuffer->bindDescriptorTable(composeDescriptorTable, VK_PIPELINE_BIND_POINT_COMPUTE)
            ->bindComputePipeline(module->composePipeline_);

        vkCmdDispatch(worldCommandBuffer->vkCommandBuffer(), (module->width_ + 15) / 16, (module->height_ + 15) / 16,
                      1);

        module->histories_.at(module->worldPipeline_.lock()->viewForSlot(frameworkContext.lock()->frameIndex)).lastRefractionHistoryFrameIndex_ = static_cast<int32_t>(currentFrameIndex);
    }
}
