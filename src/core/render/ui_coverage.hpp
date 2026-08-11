#pragma once

#include <algorithm>
#include <vulkan/vulkan_core.h>

namespace mcvr::ui {

inline bool backgroundModulation(VkBlendFactor source, VkBlendFactor destination,
                                  VkBlendOp operation, bool logicOp) noexcept {
    return !logicOp && operation == VK_BLEND_OP_ADD &&
        ((source == VK_BLEND_FACTOR_ZERO &&
          (destination == VK_BLEND_FACTOR_SRC_COLOR || destination == VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR)) ||
         (source == VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR && destination == VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR));
}

inline void coverageEquation(VkColorBlendEquationEXT &equation, bool logicOp) noexcept {
    const bool modulation = backgroundModulation(equation.srcColorBlendFactor,
        equation.dstColorBlendFactor, equation.colorBlendOp, logicOp);
    equation.srcAlphaBlendFactor = modulation ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
    equation.dstAlphaBlendFactor = modulation ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    equation.alphaBlendOp = VK_BLEND_OP_ADD;
}

inline bool sourceOverColor(VkBlendFactor source, VkBlendFactor destination,
                            VkBlendOp operation, bool logicOp) noexcept {
    return !logicOp && operation == VK_BLEND_OP_ADD &&
        (source == VK_BLEND_FACTOR_SRC_ALPHA || source == VK_BLEND_FACTOR_ONE) &&
        destination == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
}

// Porter-Duff source-over coverage. Color blending remains controlled by the
// intercepted Minecraft state; this value is the independent FG UI mask.
constexpr float accumulateCoverage(float destination, float source) noexcept {
    destination = std::clamp(destination, 0.0f, 1.0f);
    source = std::clamp(source, 0.0f, 1.0f);
    return source + destination * (1.0f - source);
}

} // namespace mcvr::ui
