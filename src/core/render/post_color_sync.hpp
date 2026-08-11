#pragma once

#include <vulkan/vulkan_core.h>

namespace mcvr {
// Tone mapping and post passes write via color attachments even when their final
// layout is PRESENT_SRC. Layout alone does not identify the producing stage.
constexpr VkPipelineStageFlags2 postColorSourceStages(VkPipelineStageFlags2 otherStages) noexcept {
    return otherStages | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
}
}
