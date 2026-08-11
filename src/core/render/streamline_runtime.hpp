#pragma once
#include "core/all_extern.hpp"
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <filesystem>
#include "optional_feature_state.hpp"

namespace mcvr {
// One owner per Vulkan device. All feature calls are made by the render thread.
class StreamlineRuntime {
public:
    static StreamlineRuntime &get();
    bool initialize(const std::filesystem::path &directory);
    void hookInstance(VkInstance instance);
    void hookDevice(VkDevice device, VkPhysicalDevice physical);
    void shutdown();
    bool ready() const { return initialized_; }
    bool supported(sl::Feature feature) const;
    sl::FrameToken *frame();
    void beginFrame(int reflexMode, uint32_t fpsLimit = 0);
    bool pacesFrames() const { return pacing_; }
    void marker(sl::PCLMarker marker);
    uint32_t allocateViewport();
    bool check(sl::Result result, const char *operation);
    OptionalFeatureLoadResult setFGLoaded(bool loaded);
    void disableFG();
    uint32_t presentedFrames();

#define MCVR_SL_API(name) PFun_##name *name{};
    MCVR_SL_API(slGetFeatureFunction)
    MCVR_SL_API(slGetFeatureRequirements)
    MCVR_SL_API(slIsFeatureSupported)
    MCVR_SL_API(slGetNewFrameToken)
    MCVR_SL_API(slSetConstants)
    MCVR_SL_API(slSetTagForFrame)
    MCVR_SL_API(slEvaluateFeature)
    MCVR_SL_API(slFreeResources)
    MCVR_SL_API(slSetFeatureLoaded)
    MCVR_SL_API(slDLSSSetOptions)
    MCVR_SL_API(slDLSSGetOptimalSettings)
    MCVR_SL_API(slDLSSDSetOptions)
    MCVR_SL_API(slDLSSDGetOptimalSettings)
    MCVR_SL_API(slDLSSGSetOptions)
    MCVR_SL_API(slDLSSGGetState)
    MCVR_SL_API(slReflexSetOptions)
    MCVR_SL_API(slReflexSleep)
    MCVR_SL_API(slPCLSetMarker)
#undef MCVR_SL_API
private:
    bool initialized_ = false;
    void *library_{};
    VkPhysicalDevice physical_{};
    PFN_vkGetInstanceProcAddr instanceProc_{};
    PFN_vkGetDeviceProcAddr deviceProc_{};
    PFun_slShutdown *shutdown_{};
    sl::FrameToken *token_{};
    uint32_t nextViewport_ = 1;
    int reflexMode_ = -1;
    bool pacing_ = false;
    bool fgLoaded_ = true;
};
sl::float4x4 slMatrix(const glm::mat4 &matrix);
sl::Constants slConstants(glm::uvec2 size, glm::vec2 jitter, const glm::mat4 &view,
    const glm::mat4 &projection, const glm::mat4 &clipToPrevious, bool reset);
}
