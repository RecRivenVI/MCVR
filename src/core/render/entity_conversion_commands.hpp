#pragma once
#include <volk.h>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace mcvr {
// Shared by the actual renderer and GPU fixture. Inputs are immutable per batch;
// their upload->compute dependency is supplied by Buffers::performQueuedUpload.
inline void recordEntityConversion(VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout,
                                   VkDescriptorSet set, uint32_t tiles, uint32_t maxGroups,
                                   VkPipelineStageFlags consumers) {
    if (!maxGroups) throw std::invalid_argument("Zero entity conversion dispatch capacity");
    if (!tiles) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    for (uint32_t first = 0; first < tiles;) {
        const uint32_t count = std::min(maxGroups, tiles - first);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(first), &first);
        vkCmdDispatch(cmd, count, 1, 1);
        first += count;
    }
    // AS vertex/index inputs use SHADER_READ, not ACCELERATION_STRUCTURE_READ.
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, consumers,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}
}
