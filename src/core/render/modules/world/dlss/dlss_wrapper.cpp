#include "core/logging.hpp"
#include "core/render/streamline_evaluate_result.hpp"
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * MODIFICATIONS and INTEGRATION:
 *
 * Copyright (c) 2026 Radiance
 *
 * This file has been modified from its original version to be integrated into
 * Radiance Mod.
 *
 * Modifications include:
 * - Integration with Radiance Mod's vulkan rendering system.
 * - Refactoring of APIs, introducing RAII and supporting shared_ptr.
 *
 * These modifications are licensed under the GNU General Public License
 * as published by the Free Software Foundation; either version 3 of the License,
 * or (at your option) any later version.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "dlss_wrapper.hpp"
#include "core/render/renderer.hpp"
#include "core/vulkan/all_core_vulkan.hpp"
#include <iostream>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sl_helpers.h>

namespace {
auto &runtime() { return mcvr::StreamlineRuntime::get(); }
NVSDK_NGX_Result result(bool ok) { return ok ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_FeatureNotSupported; }
sl::DLSSMode mode(NVSDK_NGX_PerfQuality_Value quality) {
    switch (quality) {
    case NVSDK_NGX_PerfQuality_Value_MaxPerf: return sl::DLSSMode::eMaxPerformance;
    case NVSDK_NGX_PerfQuality_Value_Balanced: return sl::DLSSMode::eBalanced;
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return sl::DLSSMode::eUltraPerformance;
    case NVSDK_NGX_PerfQuality_Value_DLAA: return sl::DLSSMode::eDLAA;
    default: return sl::DLSSMode::eMaxQuality;
    }
}
template<class Options, class Preset> void setOptions(Options &o, VkExtent2D size, NVSDK_NGX_PerfQuality_Value quality, Preset preset) {
    o.mode=mode(quality); o.outputWidth=size.width; o.outputHeight=size.height;
    o.dlaaPreset=o.qualityPreset=o.balancedPreset=o.performancePreset=o.ultraPerformancePreset=o.ultraQualityPreset=preset;
}
}
NVSDK_NGX_Result NgxContext::init(const NgxInitInfo &info) { device_=info.device; return result(runtime().ready()); }
NgxContext::~NgxContext() = default;
void NgxContext::deinit() { device_.reset(); }
NVSDK_NGX_Result NgxContext::queryDlssRRAvailable() { return result(runtime().supported(sl::kFeatureDLSS_RR)); }
NVSDK_NGX_Result NgxContext::queryDlssSRAvailable() { return result(runtime().supported(sl::kFeatureDLSS)); }
NVSDK_NGX_Result NgxContext::queryDlssFGAvailable() { return result(runtime().supported(sl::kFeatureDLSS_G) && runtime().supported(sl::kFeatureReflex)); }
// The interposer's create-instance/create-device proxies merge SDK requirements.
NVSDK_NGX_Result NgxContext::getDlssRRRequiredInstanceExtensions(std::vector<VkExtensionProperties>&, NVSDK_NGX_Feature) { return result(runtime().ready()); }
NVSDK_NGX_Result NgxContext::getDlssRRRequiredDeviceExtensions(std::shared_ptr<vk::Instance>, std::shared_ptr<vk::PhysicalDevice>, std::vector<VkExtensionProperties>&, NVSDK_NGX_Feature) { return result(runtime().ready()); }
NVSDK_NGX_Result NgxContext::querySupportedDlssInputSizes(const QuerySizeInfo &q, SupportedSizes &sizes) {
    auto &sl=runtime();
    if (q.rayReconstruction) {
        if (!sl.slDLSSDGetOptimalSettings) return result(false);
        sl::DLSSDOptions o{}; setOptions(o,q.outputSize,q.quality,static_cast<sl::DLSSDPreset>(Renderer::options.dlssRrModel));
        sl::DLSSDOptimalSettings s{};
        if (!sl.check(sl.slDLSSDGetOptimalSettings(o,s),"RR optimal size")) return result(false);
        sizes={{s.renderWidthMin,s.renderHeightMin},{s.renderWidthMax,s.renderHeightMax},{s.optimalRenderWidth,s.optimalRenderHeight}};
    } else {
        if (!sl.slDLSSGetOptimalSettings) return result(false);
        sl::DLSSOptions o{}; setOptions(o,q.outputSize,q.quality,static_cast<sl::DLSSPreset>(Renderer::options.dlssSrModel));
        sl::DLSSOptimalSettings s{};
        if (!sl.check(sl.slDLSSGetOptimalSettings(o,s),"SR optimal size")) return result(false);
        sizes={{s.renderWidthMin,s.renderHeightMin},{s.renderWidthMax,s.renderHeightMax},{s.optimalRenderWidth,s.optimalRenderHeight}};
    }
    return result(sizes.optimalSize.width && sizes.optimalSize.height);
}
NVSDK_NGX_Result NgxContext::initDlssRR(const DlssRRInitInfo &info, std::shared_ptr<vk::CommandPool> pool, std::shared_ptr<DlssRR> dlss) {
    return dlss->init(device_,pool,nullptr,info);
}
NVSDK_NGX_Result DlssRR::init(std::shared_ptr<vk::Device> device, std::shared_ptr<vk::CommandPool>, NVSDK_NGX_Parameter*, const NgxContext::DlssRRInitInfo &info) {
    deinit(); m_device=device; m_inputSize=info.inputSize; m_outputSize=info.outputSize; m_rayReconstruction=info.rayReconstruction;
    m_lastFailure.clear(); m_lastFailureStage.clear(); m_lastFailureCode=sl::Result::eOk;
    m_viewport=runtime().allocateViewport();
    auto &sl=runtime(); const sl::ViewportHandle viewport(m_viewport);
    bool ok=false;
    if (m_rayReconstruction && sl.slDLSSDSetOptions) {
        sl::DLSSDOptions o{}; setOptions(o,m_outputSize,info.quality,static_cast<sl::DLSSDPreset>(Renderer::options.dlssRrModel));
        o.normalRoughnessMode=sl::DLSSDNormalRoughnessMode::ePacked;
        o.worldToCameraView=mcvr::slMatrix(glm::mat4(1)); o.cameraViewToWorld=o.worldToCameraView;
        m_rrOptions=o;
        const auto setResult=sl.slDLSSDSetOptions(viewport,o);
        ok=setResult==sl::Result::eOk;
        if (!ok) return recordFailure("RR options",setResult);
    } else if (!m_rayReconstruction && sl.slDLSSSetOptions) {
        sl::DLSSOptions o{}; setOptions(o,m_outputSize,info.quality,static_cast<sl::DLSSPreset>(Renderer::options.dlssSrModel));
        const auto setResult=sl.slDLSSSetOptions(viewport,o);
        ok=setResult==sl::Result::eOk;
        if (!ok) return recordFailure("SR options",setResult);
    }
    mcvr::log::info("DlssWrapper") << "[Streamline] " << (m_rayReconstruction?"RR":"SR") << " viewport=" << m_viewport
        << " input=" << m_inputSize.width << 'x' << m_inputSize.height << " output=" << m_outputSize.width << 'x' << m_outputSize.height << std::endl;
    return result(ok);
}
void DlssRR::deinit() {
    // SetOptions only records configuration. Streamline does not create feature
    // resources until the first successful evaluation, and rejects attempts to
    // free a configured-but-never-evaluated viewport.
    if (m_featureEvaluated && m_viewport && runtime().ready()) runtime().check(runtime().slFreeResources(m_rayReconstruction?sl::kFeatureDLSS_RR:sl::kFeatureDLSS,sl::ViewportHandle(m_viewport)),"free reconstruction viewport");
    m_viewport=0; m_device.reset(); m_images={}; m_resetPending=true; m_lastFrame=UINT32_MAX; m_featureEvaluated=false;
}
DlssRR::~DlssRR() { deinit(); }
void DlssRR::setResource(DlssResource resourceId, std::shared_ptr<vk::DeviceLocalImage> image) { m_images[resourceId]=std::move(image); }
void DlssRR::resetResource(DlssResource resourceId) { m_images[resourceId].reset(); }
void DlssRR::requestHistoryReset() { m_resetPending=true; }
NVSDK_NGX_Result DlssRR::recordFailure(const char *stage, sl::Result failure, uint32_t frame) {
    const bool firstOccurrence = m_lastFailureCode != failure || m_lastFailureStage != stage;
    std::ostringstream description;
    description << stage << ": " << sl::getResultAsStr(failure) << " (" << int(failure) << ')'
        << ", feature=" << (m_rayReconstruction ? "DLSS-RR" : "DLSS-SR")
        << ", viewport=" << m_viewport;
    if (frame != UINT32_MAX) description << ", frame=" << frame;
    description << ", input=" << m_inputSize.width << 'x' << m_inputSize.height
        << ", output=" << m_outputSize.width << 'x' << m_outputSize.height;
    m_lastFailure=description.str();
    m_lastFailureCode=failure;
    m_lastFailureStage=stage;
    if (firstOccurrence) {
        runtime().check(failure,stage);
        std::ofstream diagnostic("radiance-streamline.log",std::ios::app);
        if (diagnostic) diagnostic << m_lastFailure << '\n';
    }
    return result(false);
}
NVSDK_NGX_Result DlssRR::denoise(std::shared_ptr<vk::CommandBuffer> cmd, glm::uvec2 size, glm::vec2 jitter,
    const glm::mat4 &view, const glm::mat4 &projection, bool reset) {
    auto &sl=runtime(); auto token=sl.frame();
    if (!token || !m_viewport) {
        m_lastFailure="reconstruction precondition failed: missing frame token or viewport";
        return result(false);
    }
    // A cached Ponder scene may be drawn twice within one real frame. Its first
    // reconstruction already produced the output; never advance that history twice.
    if (m_lastFrame==uint32_t(*token)) return result(true);
    const sl::ViewportHandle viewport(m_viewport);
    if (m_rayReconstruction) {
        m_rrOptions.worldToCameraView=mcvr::slMatrix(view); m_rrOptions.cameraViewToWorld=mcvr::slMatrix(glm::inverse(view));
        const auto setResult=sl.slDLSSDSetOptions(viewport,m_rrOptions);
        if (setResult!=sl::Result::eOk) return recordFailure("RR camera options",setResult,uint32_t(*token));
    }
    const auto previous=m_previousProjection*m_previousView*glm::inverse(view)*glm::inverse(projection);
    auto constants=mcvr::slConstants(size,jitter,view,projection,previous,reset||m_resetPending);
    const auto constantsResult=sl.slSetConstants(constants,*token,viewport);
    if (constantsResult!=sl::Result::eOk) return recordFailure("reconstruction constants",constantsResult,uint32_t(*token));
    const sl::BufferType types[]{sl::kBufferTypeScalingInputColor,sl::kBufferTypeScalingOutputColor,
        sl::kBufferTypeAlbedo,sl::kBufferTypeSpecularAlbedo,sl::kBufferTypeNormalRoughness,
        sl::kBufferTypeMotionVectors,m_rayReconstruction?sl::kBufferTypeLinearDepth:sl::kBufferTypeDepth,sl::kBufferTypeSpecularHitDistance};
    std::array<sl::Resource,RESOURCE_NUM> resources{};
    std::vector<sl::ResourceTag> tags; tags.reserve(RESOURCE_NUM);
    for (unsigned i=0;i<RESOURCE_NUM;++i) {
        if (!m_images[i]) continue;
        if (!m_rayReconstruction && i!=RESOURCE_COLOR_IN && i!=RESOURCE_COLOR_OUT && i!=RESOURCE_MOTIONVECTOR && i!=RESOURCE_LINEARDEPTH) continue;
        auto &image=*m_images[i]; auto &r=resources[i];
        r=sl::Resource(sl::ResourceType::eTex2d,(void*)image.vkImage(),nullptr,(void*)image.vkImageView(),image.imageLayout());
        r.width=image.width(); r.height=image.height(); r.nativeFormat=image.vkFormat(); r.mipLevels=1; r.arrayLayers=1; r.flags=0; r.usage=VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        const sl::Extent extent{0,0,r.width,r.height};
        tags.emplace_back(&r,types[i],sl::ResourceLifecycle::eValidUntilEvaluate,&extent);
    }
    const auto tagsResult=sl.slSetTagForFrame(*token,viewport,tags.data(),uint32_t(tags.size()),(sl::CommandBuffer*)cmd->vkCommandBuffer());
    if (tagsResult!=sl::Result::eOk) return recordFailure("reconstruction tags",tagsResult,uint32_t(*token));
    const sl::BaseStructure *inputs[]{&viewport};
    const auto evaluateResult=sl.slEvaluateFeature(m_rayReconstruction?sl::kFeatureDLSS_RR:sl::kFeatureDLSS,*token,inputs,1,(sl::CommandBuffer*)cmd->vkCommandBuffer());
    if (!mcvr::streamlineEvaluationCompleted(evaluateResult))
        return recordFailure("reconstruction evaluate",evaluateResult,uint32_t(*token));
    // The SDK already logs its budget warning. Retain successful output/history
    // and, critically, register the feature for normal viewport resource release.
    m_featureEvaluated=true;
    m_lastFailure.clear(); m_lastFailureStage.clear(); m_lastFailureCode=sl::Result::eOk;
    m_resetPending=false; m_lastFrame=uint32_t(*token); m_previousView=view; m_previousProjection=projection;
    return result(true);
}
std::string getNGXResultString(NVSDK_NGX_Result value) { return std::to_string(static_cast<unsigned>(value)); }
NVSDK_NGX_Result checkNgxResult(NVSDK_NGX_Result value,const char*,int) { return value; }
