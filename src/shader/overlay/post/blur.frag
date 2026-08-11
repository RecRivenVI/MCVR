#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require

#include "common/shared.hpp"

layout(set = 0, binding = 1) uniform sampler2D frame;
layout(std140, set = 1, binding = 1) uniform PostUniform {
    OverlayPostUBO ubo;
};

layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec2 sampleStep;

layout(location = 0) out vec4 fragColor;

// This shader relies on GL_LINEAR sampling to reduce the amount of texture samples in half.
// Instead of sampling each pixel position with a step of 1 we sample between pixels with a step of 2.
// In the end we sample the last pixel with a half weight, since the amount of pixels to sample is always odd (actualRadius * 2 + 1).
#include "shader/overlay/post/blur_kernel.glsl"
void main() {
    fragColor = radianceBlur(frame, texCoord, sampleStep, round(ubo.radius * ubo.radiusMultiplier));
}
