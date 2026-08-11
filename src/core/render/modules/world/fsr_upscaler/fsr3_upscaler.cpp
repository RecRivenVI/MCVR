#include "core/logging.hpp"
#include "core/failure_state.hpp"
#include "fsr3_upscaler.hpp"
#include "core/render/modules/world/fsr_upscaler/fsr_setup.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>

#ifdef MCVR_ENABLE_FFX_UPSCALER
#    ifdef _WIN32
#        include <windows.h>
#        define FFX_API_ENTRY
#        pragma warning(push)
#        pragma warning(disable : 4005)
#    endif

#    include <ffx_api/ffx_api.hpp>
#    include <ffx_api/ffx_upscale.hpp>
#    include <ffx_api/vk/ffx_api_vk.hpp>

#    ifdef _WIN32
#        pragma warning(pop)
#    endif

#    include <volk.h>

static uint32_t toFFXQualityMode(mcvr::UpscalerQualityMode mode) {
    switch (mode) {
        case mcvr::UpscalerQualityMode::NativeAA: return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
        case mcvr::UpscalerQualityMode::Quality: return FFX_UPSCALE_QUALITY_MODE_QUALITY;
        case mcvr::UpscalerQualityMode::Balanced: return FFX_UPSCALE_QUALITY_MODE_BALANCED;
        case mcvr::UpscalerQualityMode::Performance: return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
        case mcvr::UpscalerQualityMode::UltraPerformance: return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
        default: return FFX_UPSCALE_QUALITY_MODE_QUALITY;
    }
}

static const char *toFFXReturnCodeString(ffx::ReturnCode code) {
    switch (code) {
        case ffx::ReturnCode::Ok: return "Ok";
        case ffx::ReturnCode::Error: return "Error";
        case ffx::ReturnCode::ErrorUnknownDesctype: return "ErrorUnknownDesctype";
        case ffx::ReturnCode::ErrorRuntimeError: return "ErrorRuntimeError";
        case ffx::ReturnCode::ErrorNoProvider: return "ErrorNoProvider";
        case ffx::ReturnCode::ErrorMemory: return "ErrorMemory";
        case ffx::ReturnCode::ErrorParameter: return "ErrorParameter";
        default: return "Unknown";
    }
}
#endif // MCVR_ENABLE_FFX_UPSCALER

namespace mcvr {

FSR3Upscaler::FSR3Upscaler() {
#ifdef MCVR_ENABLE_FFX_UPSCALER
    m_fsrContext = nullptr;
#endif
}

FSR3Upscaler::~FSR3Upscaler() {
    destroy();
}

bool FSR3Upscaler::isAvailable() const {
#ifdef MCVR_ENABLE_FFX_UPSCALER
    return true;
#else
    return false;
#endif
}

bool FSR3Upscaler::initialize(const UpscalerConfig &config) {
#ifdef DEBUG
    mcvr::log::debug("Fsr3Upscaler") << "Initialize requested" << std::endl;
#endif

#ifndef MCVR_ENABLE_FFX_UPSCALER
    (void)config;
    mcvr::log::error("Fsr3Upscaler") << "FSR3Upscaler::initialize: FSR3 support not compiled in" << std::endl;
    return false;
#else
    if (m_initialized) {
#    ifdef DEBUG
        mcvr::log::debug("Fsr3Upscaler") << "Destroying the previous context" << std::endl;
#    endif
        destroy();
    }

#    ifdef DEBUG
    mcvr::log::debug("Fsr3Upscaler") << "Assigning Vulkan handles" << std::endl;
#    endif
    m_device = config.device;
    m_physicalDevice = config.physicalDevice;
#    ifdef DEBUG
    mcvr::log::debug("Fsr3Upscaler") << "Device=" << (void *)m_device << std::endl;
#    endif

    m_commandPool = config.commandPool;
    m_graphicsQueue = config.graphicsQueue;
    m_graphicsQueueFamily = config.graphicsQueueFamily;
    m_config = config;

    m_renderWidth = config.maxRenderWidth;
    m_renderHeight = config.maxRenderHeight;
    m_displayWidth = config.maxDisplayWidth;
    m_displayHeight = config.maxDisplayHeight;

#    ifdef DEBUG
    mcvr::log::debug("Fsr3Upscaler") << "Init config: render=" << m_renderWidth << "x" << m_renderHeight
              << " display=" << m_displayWidth << "x" << m_displayHeight
              << " quality=" << static_cast<int>(config.qualityMode) << " hdr=" << config.hdr
              << " depthInverted=" << config.depthInverted << " depthInfinite=" << config.depthInfinite
              << " autoExposure=" << config.autoExposure << " sharpening=" << config.enableSharpening
              << " sharpness=" << config.sharpness << std::endl;
#    endif

#    ifdef DEBUG
    mcvr::log::debug("Fsr3Upscaler") << "Creating FFX context" << std::endl;
#    endif
    if (!createContext()) {
        mcvr::log::error("Fsr3Upscaler") << "Failed to create FFX context" << std::endl;
        return false;
    }

    ffx::QueryDescUpscaleGetJitterPhaseCount jitterQuery{};
    jitterQuery.renderWidth = m_renderWidth;
    jitterQuery.displayWidth = m_displayWidth;
    jitterQuery.pOutPhaseCount = &m_jitterPhaseCount;
    ffx::Query(reinterpret_cast<ffx::Context &>(m_fsrContext), jitterQuery);

    m_jitterIndex = 0;
    m_initialized = true;

#    ifdef DEBUG
    mcvr::log::debug("Fsr3Upscaler") << "Initialized: render=" << m_renderWidth << "x" << m_renderHeight << ", display=" << m_displayWidth << "x"
              << m_displayHeight << std::endl;
#    endif

    m_debugLogged = false;

    return true;
#endif
}

bool FSR3Upscaler::createContext() {
#ifndef MCVR_ENABLE_FFX_UPSCALER
    return false;
#else
#    ifdef DEBUG
    mcvr::log::debug("Fsr3Upscaler") << "Entering FFX context creation" << std::endl;
#    endif

    // Query available providers for debug visibility, but let FFX choose the best one.
    std::vector<uint64_t> fsrVersionIds;
    std::vector<const char *> fsrVersionNames;
    {
        ffx::QueryDescGetVersions versionQuery{};
        versionQuery.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
#    ifdef _WIN32
        // On Windows, FFX SDK assumes 'device' is ID3D12Device* and crashes when passed a VkDevice.
        // Passing nullptr avoids this check and safely uses the internal FSR3 provider.
        versionQuery.device = nullptr;
#    else
        versionQuery.device = m_device;
#    endif
        uint64_t versionCount = 0;
        versionQuery.outputCount = &versionCount;

        ffxQuery(nullptr, &versionQuery.header);

        if (versionCount > 0) {
            fsrVersionIds.resize(versionCount);
            fsrVersionNames.resize(versionCount);
            versionQuery.versionIds = fsrVersionIds.data();
            versionQuery.versionNames = fsrVersionNames.data();
            versionQuery.outputCount = &versionCount;
            ffxQuery(nullptr, &versionQuery.header);

#    ifdef DEBUG
            mcvr::log::info("Fsr3Upscaler") << "FSR3: Found " << versionCount << " available FSR version(s):" << std::endl;
            for (uint64_t i = 0; i < versionCount; ++i) {
                mcvr::log::info("Fsr3Upscaler") << "  Version[" << i << "] = 0x" << std::hex << fsrVersionIds[i] << std::dec;
                if (i < fsrVersionNames.size() && fsrVersionNames[i] != nullptr) {
                    mcvr::log::info("Fsr3Upscaler") << " (" << fsrVersionNames[i] << ")";
                }
                mcvr::log::info("Fsr3Upscaler") << std::endl;
            }
#    endif

        } else {
            mcvr::log::error("Fsr3Upscaler") << "FSR3: No alternative versions available" << std::endl;
        }
    }

    // Check for required Vulkan extensions
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extensionCount, extensions.data());

    auto hasExtension = [&](const char *name) {
        return std::any_of(extensions.begin(), extensions.end(),
                           [&](const VkExtensionProperties &ext) { return strcmp(ext.extensionName, name) == 0; });
    };

#    ifdef DEBUG
    mcvr::log::info("Fsr3Upscaler") << "FSR3: Checking required extensions..." << std::endl;
#    endif

    const char *requiredExtensions[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
                                        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
                                        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME};

    for (const char *ext : requiredExtensions) {
        bool available = hasExtension(ext);
#    ifdef DEBUG
        mcvr::log::error("Fsr3Upscaler") << "  " << ext << ": " << (available ? "OK" : "MISSING") << std::endl;
#    endif
        if (!available && strcmp(ext, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0) {
            mcvr::log::error("Fsr3Upscaler") << "FSR3: Missing required extension: " << ext << std::endl;
        }
    }

    // Create Vulkan backend use volk's loaded function pointers via our wrapper
    ffx::CreateBackendVKDesc backendDesc{};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backendDesc.vkDevice = m_device;
    backendDesc.vkPhysicalDevice = m_physicalDevice;

    PFN_vkGetDeviceProcAddr deviceProcAddr =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(mcvr::fsr::customVkGetDeviceProcAddr);
    if (deviceProcAddr == nullptr) {
        mcvr::log::error("Fsr3Upscaler") << "FSR3 ERROR: customVkGetDeviceProcAddr is NULL!" << std::endl;
        return false;
    }

    PFN_vkCreateComputePipelines testCreateComputePipelines =
        (PFN_vkCreateComputePipelines)deviceProcAddr(m_device, "vkCreateComputePipelines");
    PFN_vkCmdDispatch testCmdDispatch = (PFN_vkCmdDispatch)deviceProcAddr(m_device, "vkCmdDispatch");
    PFN_vkCreateSampler testCreateSampler = (PFN_vkCreateSampler)deviceProcAddr(m_device, "vkCreateSampler");

#    ifdef DEBUG
    mcvr::log::error("Fsr3Upscaler") << "FSR3: Function pointer check:" << std::endl;
    mcvr::log::error("Fsr3Upscaler") << "  vkCreateComputePipelines: " << (void *)testCreateComputePipelines << std::endl;
    mcvr::log::error("Fsr3Upscaler") << "  vkCmdDispatch: " << (void *)testCmdDispatch << std::endl;
    mcvr::log::error("Fsr3Upscaler") << "  vkCreateSampler: " << (void *)testCreateSampler << std::endl;
#    endif

    if (testCreateComputePipelines == nullptr || testCmdDispatch == nullptr) {
        mcvr::log::error("Fsr3Upscaler") << "FSR3 ERROR: Failed to get required Vulkan function pointers" << std::endl;
        return false;
    }

    backendDesc.vkDeviceProcAddr = deviceProcAddr;
#    ifdef DEBUG
    mcvr::log::error("Fsr3Upscaler") << "FSR3: vkDeviceProcAddr = " << reinterpret_cast<void *>(deviceProcAddr) << std::endl;
#    endif

    ffx::CreateContextDescUpscale createFsr{};
    createFsr.maxUpscaleSize.width = m_displayWidth;
    createFsr.maxUpscaleSize.height = m_displayHeight;
    createFsr.maxRenderSize.width = m_renderWidth;
    createFsr.maxRenderSize.height = m_renderHeight;

    createFsr.flags = 0;
    if (m_config.hdr) { createFsr.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE; }
    if (m_config.depthInfinite) { createFsr.flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE; }
    if (m_config.depthInverted) { createFsr.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED; }
    if (m_config.autoExposure) { createFsr.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE; }

    createFsr.fpMessage = mcvr::fsr::messageCallback;

    m_fsrContext = nullptr;
    ffx::ReturnCode retCode = ffx::CreateContext(reinterpret_cast<ffx::Context &>(m_fsrContext), nullptr, createFsr,
                                                 backendDesc);

    if (retCode == ffx::ReturnCode::Ok) {
        ffx::InitHelper<ffxQueryGetProviderVersion> providerQuery{};
        if (ffx::Query(reinterpret_cast<ffx::Context &>(m_fsrContext), providerQuery) == ffx::ReturnCode::Ok &&
            providerQuery.versionName != nullptr) {
            mcvr::log::info("Fsr3Upscaler") << "FSR3: Active provider = " << providerQuery.versionName << " (0x" << std::hex
                      << providerQuery.versionId << std::dec << ")" << std::endl;
        }
#    ifdef DEBUG
        mcvr::log::info("Fsr3Upscaler") << "FSR3: CreateContext succeeded!" << std::endl;
#    endif
        m_contextCreated = true;
        return true;
    }

    mcvr::log::error("Fsr3Upscaler") << "FSR3: CreateContext failed with error: " << toFFXReturnCodeString(retCode) << " ("
              << static_cast<uint32_t>(retCode) << ")" << std::endl;

    mcvr::log::error("Fsr3Upscaler") << "FSR3: Device=" << m_device << std::endl;
    mcvr::log::error("Fsr3Upscaler") << "FSR3: PhysicalDevice=" << m_physicalDevice << std::endl;

    if (retCode == ffx::ReturnCode::ErrorRuntimeError) {
        mcvr::log::error("Fsr3Upscaler") << "FSR3: Runtime error - check Vulkan validation layers for details" << std::endl;
    }

    m_fsrContext = nullptr;
    m_contextCreated = false;
    return false;
#endif
}

void FSR3Upscaler::destroyContext() {
#ifdef MCVR_ENABLE_FFX_UPSCALER
    if (!m_fsrContext) {
        m_contextCreated = false;
        return;
    }

    auto framework = Renderer::try_instance() == nullptr ? nullptr : Renderer::instance().framework();
    const bool deviceLost = framework == nullptr ? mcvr::failure::isDeviceLost() : framework->isDeviceLost();
    if (m_device != VK_NULL_HANDLE && !deviceLost) {
        const VkResult idleResult = vkDeviceWaitIdle(m_device);
        if (idleResult != VK_SUCCESS && framework != nullptr) {
            framework->recordFailure(idleResult, "vkDeviceWaitIdle(FSR3 destroy)");
        }
    }

    ffx::ReturnCode result = ffx::DestroyContext(reinterpret_cast<ffx::Context &>(m_fsrContext));
    if (result != ffx::ReturnCode::Ok) {
        if (m_device != VK_NULL_HANDLE && !deviceLost) {
            const VkResult idleResult = vkDeviceWaitIdle(m_device);
            if (idleResult != VK_SUCCESS && framework != nullptr) {
                framework->recordFailure(idleResult, "vkDeviceWaitIdle(FSR3 destroy retry)");
            }
        }
        result = ffx::DestroyContext(reinterpret_cast<ffx::Context &>(m_fsrContext));
    }
    if (result != ffx::ReturnCode::Ok) {
        mcvr::log::error("Fsr3Upscaler") << "FSR3: DestroyContext failed with error: " << toFFXReturnCodeString(result) << " ("
                  << static_cast<uint32_t>(result) << ")" << std::endl;
    }

    m_fsrContext = nullptr;
    m_contextCreated = false;
#endif
}

void FSR3Upscaler::dispatch(const UpscalerInput &input) {
#ifndef MCVR_ENABLE_FFX_UPSCALER
    (void)input;
    return;
#else
    if (!m_initialized || !m_contextCreated) { return; }

    if (!m_debugLogged || input.renderWidth != m_lastRenderWidth || input.renderHeight != m_lastRenderHeight ||
        input.displayWidth != m_lastDisplayWidth || input.displayHeight != m_lastDisplayHeight) {
        m_debugLogged = true;
        m_lastRenderWidth = input.renderWidth;
        m_lastRenderHeight = input.renderHeight;
        m_lastDisplayWidth = input.displayWidth;
        m_lastDisplayHeight = input.displayHeight;

#    ifdef DEBUG
        mcvr::log::info("Fsr3Upscaler") << "[FSR3] dispatch: render=" << input.renderWidth << "x" << input.renderHeight
                  << " display=" << input.displayWidth << "x" << input.displayHeight
                  << " colorFmt=" << static_cast<int>(input.colorFormat)
                  << " depthFmt=" << static_cast<int>(input.depthFormat)
                  << " mvFmt=" << static_cast<int>(input.motionVectorFormat)
                  << " outputFmt=" << static_cast<int>(input.outputFormat)
                  << " depthFfx=" << static_cast<int>(mcvr::fsr::vkToFfxFormat(input.depthFormat)) << " jitter=("
                  << input.jitterOffsetX << "," << input.jitterOffsetY << ")" << " mvScale=("
                  << input.motionVectorScaleX << "," << input.motionVectorScaleY << ")" << " reset=" << input.reset
                  << " preExposure=" << input.preExposure << " exposure=" << (input.exposureImage != VK_NULL_HANDLE)
                  << " reactive=" << (input.reactiveImage != VK_NULL_HANDLE) << std::endl;
#    endif
    }

    auto createResource = [](VkImage image, uint32_t width, uint32_t height, VkFormat format, FfxApiResourceState state,
                             FfxApiResourceUsage usage = FFX_API_RESOURCE_USAGE_READ_ONLY) -> FfxApiResource {
        FfxApiResource resource = {};
        resource.resource = reinterpret_cast<void *>(image);
        resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        resource.description.format = mcvr::fsr::vkToFfxFormat(format);
        resource.description.width = width;
        resource.description.height = height;
        resource.description.depth = 1;
        resource.description.mipCount = 1;
        resource.description.usage = usage;
        resource.state = state;
        return resource;
    };

    const VkFormat colorFormat =
        input.colorFormat != VK_FORMAT_UNDEFINED ? input.colorFormat : VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkFormat depthFormat = input.depthFormat != VK_FORMAT_UNDEFINED ? input.depthFormat : VK_FORMAT_D32_SFLOAT;
    const VkFormat motionVectorFormat =
        input.motionVectorFormat != VK_FORMAT_UNDEFINED ? input.motionVectorFormat : VK_FORMAT_R16G16_SFLOAT;
    const VkFormat outputFormat =
        input.outputFormat != VK_FORMAT_UNDEFINED ? input.outputFormat : colorFormat;

    ffx::DispatchDescUpscale dispatchUpscale{};
    dispatchUpscale.commandList = input.commandBuffer;

    dispatchUpscale.color =
        createResource(input.colorImage, input.renderWidth, input.renderHeight, colorFormat,
                       FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);

    dispatchUpscale.depth = createResource(input.depthImage, input.renderWidth, input.renderHeight, depthFormat,
                                           FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);

    dispatchUpscale.motionVectors =
        createResource(input.motionVectorImage, input.renderWidth, input.renderHeight, motionVectorFormat,
                       FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);

    if (input.exposureImage != VK_NULL_HANDLE) {
        dispatchUpscale.exposure =
            createResource(input.exposureImage, 1, 1, VK_FORMAT_R32_SFLOAT, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    } else {
        dispatchUpscale.exposure = {};
    }

    if (input.reactiveImage != VK_NULL_HANDLE) {
        dispatchUpscale.reactive = createResource(input.reactiveImage, input.renderWidth, input.renderHeight,
                                                  VK_FORMAT_R8_UNORM, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    } else {
        dispatchUpscale.reactive = {};
    }

    dispatchUpscale.transparencyAndComposition = {};

    // Output resource
    dispatchUpscale.output = createResource(input.outputImage, input.displayWidth, input.displayHeight, outputFormat,
                                            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
                                            FFX_API_RESOURCE_USAGE_UAV);

    // Jitter offset
    dispatchUpscale.jitterOffset.x = input.jitterOffsetX;
    dispatchUpscale.jitterOffset.y = input.jitterOffsetY;

    // Motion vector scale
    dispatchUpscale.motionVectorScale.x = input.motionVectorScaleX;
    dispatchUpscale.motionVectorScale.y = input.motionVectorScaleY;

    // Resolution
    dispatchUpscale.renderSize.width = input.renderWidth;
    dispatchUpscale.renderSize.height = input.renderHeight;
    dispatchUpscale.upscaleSize.width = input.displayWidth;
    dispatchUpscale.upscaleSize.height = input.displayHeight;

    // Sharpening
    dispatchUpscale.enableSharpening = input.enableSharpening;
    dispatchUpscale.sharpness = input.sharpness;

    // Frame parameters
    dispatchUpscale.frameTimeDelta = input.frameTimeDelta;
    dispatchUpscale.preExposure = input.preExposure;
    dispatchUpscale.reset = input.reset;

    // Camera parameters
    dispatchUpscale.cameraNear = input.cameraNear;
    dispatchUpscale.cameraFar = input.cameraFar;
    dispatchUpscale.cameraFovAngleVertical = input.cameraFovVertical;
    dispatchUpscale.viewSpaceToMetersFactor = 0.0f;

    // Flags
    dispatchUpscale.flags = 0;

    // Dispatch FSR3
    static int dispatchCount = 0;
    ffx::ReturnCode retCode = ffx::Dispatch(reinterpret_cast<ffx::Context &>(m_fsrContext), dispatchUpscale);
    if (retCode != ffx::ReturnCode::Ok) {
        mcvr::log::error("Fsr3Upscaler") << "FSR3 DISPATCH FAILED: " << toFFXReturnCodeString(retCode) << " ("
                  << static_cast<uint32_t>(retCode) << ")" << std::endl;
        return;
    }
    // else {
    //     dispatchCount++;
    //     if (dispatchCount == 1 || dispatchCount % 300 == 0) {
    //         mcvr::log::error("Fsr3Upscaler") << "FSR3 dispatch OK (count: " << dispatchCount << ")" << std::endl;
    //     }
    // }

    // Increment jitter index
    m_jitterIndex++;
#endif
}

void FSR3Upscaler::resize(uint32_t renderWidth, uint32_t renderHeight, uint32_t displayWidth, uint32_t displayHeight) {
#ifndef MCVR_ENABLE_FFX_UPSCALER
    (void)renderWidth;
    (void)renderHeight;
    (void)displayWidth;
    (void)displayHeight;
#else
    if (!m_initialized) { return; }

    // Recreate context with new resolution
    m_renderWidth = renderWidth;
    m_renderHeight = renderHeight;
    m_displayWidth = displayWidth;
    m_displayHeight = displayHeight;

    destroyContext();
    createContext();

    // Re-query jitter phase count
    ffx::QueryDescUpscaleGetJitterPhaseCount jitterQuery{};
    jitterQuery.renderWidth = m_renderWidth;
    jitterQuery.displayWidth = m_displayWidth;
    jitterQuery.pOutPhaseCount = &m_jitterPhaseCount;
    ffx::Query(reinterpret_cast<ffx::Context &>(m_fsrContext), jitterQuery);

    m_jitterIndex = 0;
#endif
}

void FSR3Upscaler::destroy() {
#ifdef MCVR_ENABLE_FFX_UPSCALER
    destroyContext();

    m_device = VK_NULL_HANDLE;
    m_physicalDevice = VK_NULL_HANDLE;
    m_commandPool = VK_NULL_HANDLE;
    m_graphicsQueue = VK_NULL_HANDLE;
    m_initialized = false;
#endif
}

} // namespace mcvr
