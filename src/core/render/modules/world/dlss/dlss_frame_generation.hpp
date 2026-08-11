#pragma once
#include "core/vulkan/all_core_vulkan.hpp"
#include <atomic>
class Framework;
class FrameworkContext;
struct PipelineContext;
// Inputs are produced only on the presenting queue. The default SL queue mode
// orders subsequent GPU reuse; teardown follows proxy device-wait-idle.
class DlssFrameGeneration {
public:
    ~DlssFrameGeneration();
    void captureHudless(Framework&,FrameworkContext&,const std::shared_ptr<vk::DeviceLocalImage>&);
    void blurHudless(Framework&, FrameworkContext&, const vk::Data::OverlayPostUBO&,
                     const std::shared_ptr<vk::DeviceLocalImage>&);
    std::shared_ptr<vk::DeviceLocalImage> record(Framework&,FrameworkContext&,const std::shared_ptr<PipelineContext>&);
    bool generated() const { return generated_; }
    std::shared_ptr<vk::DeviceLocalImage> hudless(uint32_t frameIndex) const {
        return frameIndex < hudless_.size() ? hudless_[frameIndex] : nullptr;
    }
private:
    bool initialize(Framework&,const std::shared_ptr<vk::DeviceLocalImage>&,const std::shared_ptr<vk::DeviceLocalImage>&);
    std::shared_ptr<vk::Device> device_;
    std::shared_ptr<vk::DeviceLocalImage> real_,depth_,uiAlpha_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> hudless_;
    std::vector<std::shared_ptr<vk::DescriptorTable>> coverageResetTables_;
    std::shared_ptr<vk::ComputePipeline> coverageResetPipeline_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> hudlessBlurScratch_;
    std::vector<std::shared_ptr<vk::DeviceLocalImage>> hudlessBlurWeighted_;
    std::vector<std::shared_ptr<vk::DescriptorTable>> hudlessWeightTables_;
    std::shared_ptr<vk::ComputePipeline> hudlessWeightPipeline_;
    std::vector<std::shared_ptr<vk::DescriptorTable>> hudlessBlurTables_;
    std::shared_ptr<vk::ComputePipeline> hudlessBlurPipeline_;
    std::shared_ptr<vk::Sampler> hudlessBlurSampler_;
    std::vector<std::shared_ptr<vk::DescriptorTable>> depthTables_,uiTables_;
    std::shared_ptr<vk::ComputePipeline> depthPipeline_,uiPipeline_;
    bool backgroundLimitReported_ = false;
    bool attempted_=false,failed_=false,reset_=true,generated_=false;
    glm::mat4 previousProjection_{1},previousView_{1};
    glm::dvec3 previousPosition_{};
    inline static std::atomic<int> asyncError_{0};
};
