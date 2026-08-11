#pragma once
#include <vulkan/vulkan.h>
#include <span>
#include <cstdint>
#include <stdexcept>

namespace vk {
// Small pass constants are copied into the command stream, not a shared mapped
// staging allocation which another view/frame could overwrite before submission.
inline void recordInlineUpdate(VkCommandBuffer commands, VkBuffer buffer, std::span<const uint32_t> words) {
    if (!commands || !buffer || words.empty() || words.size_bytes() > 65536)
        throw std::invalid_argument("Invalid inline buffer update");
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer; barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
    vkCmdUpdateBuffer(commands, buffer, 0, words.size_bytes(), words.data());
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
}
}
