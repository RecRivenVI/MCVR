#include "core/logging.hpp"
#include "streamline_runtime.hpp"
#include "renderer.hpp"
#include <cstring>
#include <cmath>
#include <iostream>
#ifdef _WIN32
#include <sl_security.h>
#include <sl_helpers.h>
#endif

namespace mcvr {
StreamlineRuntime &StreamlineRuntime::get() { static StreamlineRuntime runtime; return runtime; }
bool StreamlineRuntime::check(sl::Result result, const char *operation) {
    if (result == sl::Result::eOk) return true;
    mcvr::log::error("StreamlineRuntime") << operation << " failed: result=" << sl::getResultAsStr(result)
        << " (" << int(result) << ')' << std::endl;
    return false;
}
bool StreamlineRuntime::initialize(const std::filesystem::path &directory) {
    if (initialized_) return true;
#ifdef _WIN32
    const auto path = std::filesystem::absolute(directory / "sl.interposer.dll");
    if (!sl::security::verifyEmbeddedSignature(path.c_str())) {
        mcvr::log::error("StreamlineRuntime") << "Interposer is missing or has an invalid signature: " << path << std::endl;
        return false;
    }
    auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) return false;
    library_ = module;
    auto init = reinterpret_cast<PFun_slInit *>(GetProcAddress(module, "slInit"));
    shutdown_ = reinterpret_cast<PFun_slShutdown *>(GetProcAddress(module, "slShutdown"));
    instanceProc_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
    deviceProc_ = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(module, "vkGetDeviceProcAddr"));
#define LOAD(name) name = reinterpret_cast<PFun_##name *>(GetProcAddress(module, #name)); if (!name) return false;
    LOAD(slGetFeatureFunction) LOAD(slGetFeatureRequirements) LOAD(slIsFeatureSupported) LOAD(slGetNewFrameToken)
    LOAD(slSetConstants) LOAD(slSetTagForFrame) LOAD(slEvaluateFeature) LOAD(slFreeResources)
    LOAD(slSetFeatureLoaded)
#undef LOAD
    if (!init || !shutdown_ || !instanceProc_ || !deviceProc_) return false;
    const auto pluginPath = std::filesystem::absolute(directory).wstring();
    const wchar_t *paths[]{pluginPath.c_str()};
    const sl::Feature features[]{sl::kFeatureDLSS, sl::kFeatureDLSS_RR, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences preferences{};
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "MCVR-0.1.5";
    preferences.projectId = "c4ce574c-74e1-4b9b-a76c-6ff964cbba85";
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
        sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.pathsToPlugins = paths; preferences.numPathsToPlugins = 1;
    preferences.featuresToLoad = features; preferences.numFeaturesToLoad = std::size(features);
    preferences.logMessageCallback = [](sl::LogType type, const char *message) {
        try {
            mcvr::log::error("StreamlineRuntime") << "[Streamline:" << int(type) << "] "
                                                   << (message ? message : "") << std::endl;
        } catch (...) {
            // Never let a C++ exception cross the Streamline callback boundary.
        }
    };
    if (!check(init(preferences, sl::kSDKVersion), "slInit")) return false;
    initialized_ = true;
    vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(instanceProc_(nullptr, "vkCreateInstance"));
    mcvr::log::info("StreamlineRuntime") << "Streamline 2.14.1 initialized; OTA disabled" << std::endl;
    return true;
#else
    return false;
#endif
}
void StreamlineRuntime::hookInstance(VkInstance instance) {
    if (!initialized_) return;
#define HOOK(name) name = reinterpret_cast<PFN_##name>(instanceProc_(instance, #name));
    // The create-device proxy looks up the owning instance in the enumeration
    // proxy's map; bypassing enumeration leaves SL with a null VkInstance.
    HOOK(vkEnumeratePhysicalDevices) HOOK(vkCreateDevice) HOOK(vkDestroySurfaceKHR)
    HOOK(vkDestroyInstance)
#ifdef _WIN32
    HOOK(vkCreateWin32SurfaceKHR)
#endif
#undef HOOK
}
void StreamlineRuntime::hookDevice(VkDevice device, VkPhysicalDevice physical) {
    if (!initialized_) return;
    physical_ = physical;
#define HOOK(name) name = reinterpret_cast<PFN_##name>(deviceProc_(device, #name));
    HOOK(vkCreateSwapchainKHR) HOOK(vkDestroySwapchainKHR) HOOK(vkGetSwapchainImagesKHR)
    HOOK(vkAcquireNextImageKHR) HOOK(vkAcquireNextImage2KHR) HOOK(vkQueuePresentKHR) HOOK(vkDeviceWaitIdle)
    HOOK(vkDestroyDevice)
#undef HOOK
#define FEATURE(feature, name) { void *function{}; if (slGetFeatureFunction(feature, #name, function) == sl::Result::eOk) name = reinterpret_cast<PFun_##name *>(function); }
    FEATURE(sl::kFeatureDLSS, slDLSSSetOptions) FEATURE(sl::kFeatureDLSS, slDLSSGetOptimalSettings)
    FEATURE(sl::kFeatureDLSS_RR, slDLSSDSetOptions) FEATURE(sl::kFeatureDLSS_RR, slDLSSDGetOptimalSettings)
    FEATURE(sl::kFeatureDLSS_G, slDLSSGSetOptions) FEATURE(sl::kFeatureDLSS_G, slDLSSGGetState)
    FEATURE(sl::kFeatureReflex, slReflexSetOptions) FEATURE(sl::kFeatureReflex, slReflexSleep)
    FEATURE(sl::kFeaturePCL, slPCLSetMarker)
#undef FEATURE
    for (const auto &[feature, name] : std::initializer_list<std::pair<sl::Feature, const char *>>{
             {sl::kFeatureDLSS, "DLSS-SR"}, {sl::kFeatureDLSS_RR, "DLSS-RR"}}) {
        sl::FeatureRequirements requirements{};
        const auto requirementsResult = slGetFeatureRequirements(feature, requirements);
        if (requirementsResult == sl::Result::eOk) {
            mcvr::log::info("StreamlineRuntime") << name << " maxNumViewports="
                << requirements.maxNumViewports << " requiredTags=" << requirements.numRequiredTags << std::endl;
        } else {
            check(requirementsResult, name);
        }
    }
    // DLSS-G owns a proxy swapchain whenever its plugin is loaded, even while
    // generation mode is off. Keep it unloaded until the user actually enables
    // FG; presentation recreation will load it before creating the new swapchain.
    setFGLoaded(Renderer::options.dlssFrameGeneration);
}
bool StreamlineRuntime::supported(sl::Feature feature) const {
    if (!initialized_ || !physical_) return false;
    sl::AdapterInfo adapter{}; adapter.vkPhysicalDevice = physical_;
    return slIsFeatureSupported(feature, adapter) == sl::Result::eOk;
}
void StreamlineRuntime::shutdown() {
    if (initialized_) check(shutdown_(), "slShutdown");
    initialized_ = false; physical_ = nullptr; token_ = nullptr;
    // Keep the interposer loaded while Vulkan objects and their proxy functions exist.
}
sl::FrameToken *StreamlineRuntime::frame() {
    if (initialized_ && !token_) check(slGetNewFrameToken(token_, nullptr), "slGetNewFrameToken");
    return token_;
}
void StreamlineRuntime::beginFrame(int mode, uint32_t fpsLimit) {
    if (!initialized_) return;
    pacing_ = false;
    token_ = nullptr;
    if (!frame()) return;
    if (slReflexSetOptions && supported(sl::kFeatureReflex)) {
        sl::ReflexOptions options{}; options.mode = static_cast<sl::ReflexMode>(mode);
        options.frameLimitUs = mode > 0 && fpsLimit > 0 ? 1000000u / fpsLimit : 0;
        // Reapply after swapchain replacement, even when the selected mode is unchanged.
        if (check(slReflexSetOptions(options), "slReflexSetOptions")) {
            if (reflexMode_ != mode) mcvr::log::info("StreamlineRuntime") << "Effective Reflex mode=" << mode << std::endl;
            reflexMode_ = mode;
            if (slReflexSleep) pacing_ = check(slReflexSleep(*token_), "slReflexSleep") && mode > 0;
        }
    }
}
void StreamlineRuntime::marker(sl::PCLMarker mark) {
    if (slPCLSetMarker && frame()) check(slPCLSetMarker(mark, *token_), "slPCLSetMarker");
}
uint32_t StreamlineRuntime::allocateViewport() { return nextViewport_++; }
OptionalFeatureLoadResult StreamlineRuntime::setFGLoaded(bool loaded) {
    switch (planOptionalFeatureLoad(initialized_, slSetFeatureLoaded != nullptr, fgLoaded_, loaded)) {
    case OptionalFeatureLoadAction::NoChange:
        return OptionalFeatureLoadResult::Unchanged;
    case OptionalFeatureLoadAction::SatisfiedWithoutRuntime:
        return OptionalFeatureLoadResult::SatisfiedWithoutRuntime;
    case OptionalFeatureLoadAction::Unavailable:
        return OptionalFeatureLoadResult::Unavailable;
    case OptionalFeatureLoadAction::InvokeRuntime:
        break;
    }
    if (!check(slSetFeatureLoaded(sl::kFeatureDLSS_G, loaded), loaded ? "load FG" : "unload FG")) {
        return OptionalFeatureLoadResult::Failed;
    }
    fgLoaded_ = loaded;
    if (loaded && slGetFeatureFunction) {
        void *setOptions{};
        void *getState{};
        if (slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", setOptions) == sl::Result::eOk) {
            slDLSSGSetOptions = reinterpret_cast<PFun_slDLSSGSetOptions *>(setOptions);
        }
        if (slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", getState) == sl::Result::eOk) {
            slDLSSGGetState = reinterpret_cast<PFun_slDLSSGGetState *>(getState);
        }
    }
    mcvr::log::info("StreamlineRuntime") << "DLSS-G plugin " << (loaded ? "loaded" : "unloaded") << std::endl;
    return OptionalFeatureLoadResult::Changed;
}
void StreamlineRuntime::disableFG() {
    if (initialized_ && fgLoaded_ && slDLSSGSetOptions && supported(sl::kFeatureDLSS_G)) {
        sl::DLSSGOptions options{};
        check(slDLSSGSetOptions(sl::ViewportHandle(0), options), "FG off");
        if (token_) {
            const sl::ResourceTag tags[]{
                {nullptr,sl::kBufferTypeDepth,sl::ResourceLifecycle::eValidUntilPresent},
                {nullptr,sl::kBufferTypeMotionVectors,sl::ResourceLifecycle::eValidUntilPresent},
                {nullptr,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent},
                {nullptr,sl::kBufferTypeUIAlpha,sl::ResourceLifecycle::eValidUntilPresent}};
            check(slSetTagForFrame(*token_,sl::ViewportHandle(0),tags,4,nullptr),"clear FG tags");
        }
    }
}
uint32_t StreamlineRuntime::presentedFrames() {
    if (!initialized_ || !fgLoaded_ || !slDLSSGGetState || !supported(sl::kFeatureDLSS_G)) return 0;
    sl::DLSSGState state{};
    if (!check(slDLSSGGetState(sl::ViewportHandle(0), state, nullptr), "FG state")) return 0;
    return state.numFramesActuallyPresented;
}
sl::float4x4 slMatrix(const glm::mat4 &matrix) {
    sl::float4x4 out{};
    // GLM column-major column-vector storage is SL row-major row-vector storage.
    std::memcpy(&out, &matrix[0][0], sizeof(out)); return out;
}
sl::Constants slConstants(glm::uvec2 size, glm::vec2 jitter, const glm::mat4 &view,
    const glm::mat4 &p, const glm::mat4 &toPrevious, bool reset) {
    sl::Constants c{};
    c.cameraViewToClip=slMatrix(p); c.clipToCameraView=slMatrix(glm::inverse(p));
    c.clipToLensClip=slMatrix(glm::mat4(1)); c.clipToPrevClip=slMatrix(toPrevious);
    c.prevClipToClip=slMatrix(glm::inverse(toPrevious));
    c.jitterOffset={-jitter.x,-jitter.y}; c.mvecScale={1.f/size.x,1.f/size.y}; c.cameraPinholeOffset={0,0};
    const auto inverse=glm::inverse(view);
    c.cameraPos={inverse[3].x,inverse[3].y,inverse[3].z};
    c.cameraRight={inverse[0].x,inverse[0].y,inverse[0].z};
    c.cameraUp={inverse[1].x,inverse[1].y,inverse[1].z};
    c.cameraFwd={-inverse[2].x,-inverse[2].y,-inverse[2].z};
    c.cameraNear=std::abs(p[3][2]/p[2][2]); c.cameraFar=std::abs((p[3][2]-p[3][3])/(p[2][2]-p[2][3]));
    if (!std::isfinite(c.cameraFar)) c.cameraFar=100000.f;
    c.cameraFOV=2*std::atan(1/std::abs(p[1][1])); c.cameraAspectRatio=std::abs(p[1][1]/p[0][0]);
    c.depthInverted=sl::eFalse; c.cameraMotionIncluded=sl::eTrue; c.motionVectors3D=sl::eFalse;
    c.reset=reset?sl::eTrue:sl::eFalse; c.orthographicProjection=std::abs(p[2][3])<.5f?sl::eTrue:sl::eFalse;
    c.motionVectorsInvalidValue=65504.f;
    return c;
}
}
