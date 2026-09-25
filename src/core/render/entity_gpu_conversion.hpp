#pragma once
#include "core/vulkan/all_core_vulkan.hpp"
#include <vector>
class Framework;
struct EntityBuildData;

// Immutable per-build input and descriptors. Pipeline alone is shared. The frame
// retainer owns every recorded batch through completion, independently of CPU data.
class EntityGpuConversion : public SharedObject<EntityGpuConversion> {
public:
    EntityGpuConversion(Framework &framework, const std::vector<std::shared_ptr<EntityBuildData>> &data,
                        std::shared_ptr<vk::DeviceLocalBuffer> positions,
                        std::shared_ptr<vk::DeviceLocalBuffer> materials,
                        std::shared_ptr<vk::DeviceLocalBuffer> indices,
                        std::shared_ptr<vk::ComputePipeline> &pipeline);
    void record(Framework &framework, const std::shared_ptr<vk::CommandBuffer> &commands);
    std::shared_ptr<vk::DeviceLocalBuffer> input, jobs;
private:
    std::shared_ptr<vk::DescriptorTable> table_;
    std::shared_ptr<vk::ComputePipeline> pipeline_;
    uint32_t tiles_ = 0;
    bool recorded_ = false;
};
