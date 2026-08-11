#pragma once
#include <cstdint>
#include <span>
#include <vulkan/vulkan_core.h>
#include "core/render/geometry_policy.hpp"
namespace mcvr::faces {
// Shared with Java MaterialFaces and util/material_faces.glsl; other material bits are untouched.
inline constexpr uint32_t back = 1u << 2, front = 1u << 7, clockwise = 1u << 22;
inline constexpr uint32_t mask = back | front | clockwise;
// Backend-only provenance: a uniform CULL_FRONT instance is flipped to use the
// same hardware traversal shortcut as CULL_BACK. Undo that extra flip in software.
inline constexpr uint32_t hardwareFrontFlip = 1u << 23;
inline constexpr uint32_t pairedGeometry = 1u << 19;
constexpr uint32_t effective(uint32_t flags) {
    uint32_t faces = flags & (back | front);
    return (flags & clockwise) ? ((faces & back ? front : 0) | (faces & front ? back : 0)) : faces;
}
constexpr bool accepts(uint32_t flags, bool ccwFront) {
    return (effective(flags) & (ccwFront ? front : back)) == 0;
}
enum class QueryRole { sourceSelection, lightVisibility, radianceEndpoint };
constexpr bool acceptsTraversalHit(uint32_t flags, bool hardwareFront, QueryRole role) {
    const bool sourceFront = hardwareFront != ((flags & hardwareFrontFlip) != 0);
    // All ray roles use their own travel direction. Sidedness never implies an
    // additional transmission term at an accepted surface.
    return accepts(flags, sourceFront);
}
inline uint32_t uniform(std::span<const uint32_t> flags) {
    if (flags.empty()) return 0;
    auto first = effective(flags.front());
    if (first != back && first != front) return 0;
    for (auto f : flags)
        if (effective(f) != first) return 0;
    return first;
}
// A value snapshot for one published model; rebuild it when the material version changes.
// Scan the model once, then use the same decision for BLAS, TLAS and shader flags.
class ModelRules {
  public:
    explicit ModelRules(std::span<const uint32_t> materials, bool bilateral = mcvr::geometry::twoSided())
        : face_((bilateral || mcvr::geometry::backfaceView()) ? 0u : uniform(materials)), bilateral_(bilateral) {}

    bool needsAnyHit(uint32_t material) const {
        if (mcvr::geometry::backfaceView() && (material & pairedGeometry) != 0u) return true;
        return bilateral_ ? (material & pairedGeometry) != 0u : effective(material) != 0 && face_ == 0;
    }
    uint32_t shaderFlags(uint32_t material) const {
        return (material & ~hardwareFrontFlip) | (face_ == front ? hardwareFrontFlip : 0u);
    }
    VkGeometryInstanceFlagsKHR instanceFlags(const VkTransformMatrixKHR &t) const {
        const float d = t.matrix[0][0] * (t.matrix[1][1] * t.matrix[2][2] - t.matrix[1][2] * t.matrix[2][1]) -
                        t.matrix[0][1] * (t.matrix[1][0] * t.matrix[2][2] - t.matrix[1][2] * t.matrix[2][0]) +
                        t.matrix[0][2] * (t.matrix[1][0] * t.matrix[2][1] - t.matrix[1][1] * t.matrix[2][0]);
        VkGeometryInstanceFlagsKHR flags = face_ ? 0 : VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        if ((d < 0) != (face_ == mcvr::faces::front)) flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
        return flags;
    }

  private:
    uint32_t face_;
    bool bilateral_;
};
inline bool needsAnyHit(uint32_t flags, std::span<const uint32_t> model) {
    return ModelRules(model).needsAnyHit(flags);
}
inline VkGeometryInstanceFlagsKHR instanceFlags(std::span<const uint32_t> materials, const VkTransformMatrixKHR &t) {
    return ModelRules(materials).instanceFlags(t);
}
} // namespace mcvr::faces
