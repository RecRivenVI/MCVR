#pragma once
#include <array>

namespace mcvr::ui {
// The fragment reference rounds in GLSL. Do not quantize the radius on the host.
constexpr std::array<float,4> blurParameters(float width, float height, float dx, float dy,
                                            float radius, float multiplier) noexcept {
    return {dx/width,dy/height,radius*multiplier,1.f};
}
}
