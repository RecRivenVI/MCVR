#pragma once
#include "core/vulkan/all_core_vulkan.hpp"
#include <vector>
#include <vector>
class Framework;
struct EntityBuildData;
namespace mcvr { struct EntityDirectInputBatch; }

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
    struct SourceGroup {
        std::shared_ptr<mcvr::EntityDirectInputBatch> directOwner;
        std::shared_ptr<vk::DeviceLocalBuffer> input;
        std::shared_ptr<vk::DeviceLocalBuffer> jobs;
        std::shared_ptr<vk::DescriptorTable> table;
        uint32_t tiles = 0;
    };
    std::vector<SourceGroup> sourceGroups;
private:
    std::shared_ptr<vk::ComputePipeline> pipeline_;
    bool recorded_ = false;
};
