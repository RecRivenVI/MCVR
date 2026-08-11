#pragma once
#include <cstdint>
#include <span>
#include <vulkan/vulkan_core.h>
namespace mcvr::faces {
// Shared with Java MaterialFaces and util/material_faces.glsl; other material bits are untouched.
inline constexpr uint32_t back = 1u << 2, front = 1u << 7, clockwise = 1u << 22;
inline constexpr uint32_t mask = back | front | clockwise;
constexpr uint32_t effective(uint32_t flags) {
    uint32_t faces = flags & (back | front);
    return (flags & clockwise) ? ((faces & back ? front : 0) | (faces & front ? back : 0)) : faces;
}
constexpr bool accepts(uint32_t flags, bool ccwFront) {
    return (effective(flags) & (ccwFront ? front : back)) == 0;
}
inline uint32_t uniform(std::span<const uint32_t> flags) {
    if (flags.empty()) return 0;
    auto first = effective(flags.front());
    if (first != back && first != front) return 0;
    for (auto f : flags)
        if (effective(f) != first) return 0;
    return first;
}
inline bool needsAnyHit(uint32_t flags, std::span<const uint32_t> model) {
    return effective(flags) != 0 && uniform(model) == 0;
}
inline VkGeometryInstanceFlagsKHR instanceFlags(std::span<const uint32_t> materials, const VkTransformMatrixKHR &t) {
    const auto face = mcvr::faces::uniform(materials);
    const float d = t.matrix[0][0] * (t.matrix[1][1] * t.matrix[2][2] - t.matrix[1][2] * t.matrix[2][1]) -
                    t.matrix[0][1] * (t.matrix[1][0] * t.matrix[2][2] - t.matrix[1][2] * t.matrix[2][0]) +
                    t.matrix[0][2] * (t.matrix[1][0] * t.matrix[2][1] - t.matrix[1][1] * t.matrix[2][0]);
    VkGeometryInstanceFlagsKHR flags = face ? 0 : VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    if ((d < 0) != (face == mcvr::faces::front)) flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
    return flags;
}
} // namespace mcvr::faces
