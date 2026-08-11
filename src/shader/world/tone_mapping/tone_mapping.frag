#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require

#include "common/shared.hpp"

layout(set = 0, binding = 0) uniform sampler2D HDR;
layout(set = 0, binding = 3) uniform sampler2D textures[4096];
layout(set = 0, binding = 4) uniform WorldUniformBuffer {
    WorldUBO worldUBO;
};

layout(set = 0, binding = 2) readonly buffer ExposureBuffer {
    float exposure;
    float avgLogLum;
    uint historyValid;
    uint padding;
}
expData;

layout(push_constant) uniform PushConstant {
    float log2Min;
    float log2Max;
    float epsilon;
    float lowPercent;
    float highPercent;
    float middleGrey;
    float dt;
    float speedUp;
    float speedDown;
    float minExposure;
    float maxExposure;
    float manualExposure;
    float exposureBias;
    float whitePoint;
    float saturation;
    int toneMappingMethod;
    int autoExposure;
    int clampOutput;
    int exposureMeteringMode;
    float centerMeteringPercent;
    float padding0;
    float padding1;
    float padding2;
}
pc;

layout(location = 0) in vec2 texCoord;

layout(location = 0) out vec4 fragColor;

// https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral
vec3 pbrNeutralToneMap(vec3 color) {
    float startCompression = 0.76;
    float desaturation = 0.01;

    float x = min(color.r, min(color.g, color.b));
    float offset = (x < 0.08) ? (x - 6.25 * x * x) : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, newPeak * vec3(1.0), g);
}

// all following
// https://64.github.io/tonemapping/
vec3 reinhardToneMap(vec3 color) {
    return color / (1.0 + color);
}

vec3 reinhardWhitePointToneMap(vec3 color, float whitePoint) {
    float w2 = max(whitePoint * whitePoint, 1e-6);
    return (color * (1.0 + color / w2)) / (1.0 + color);
}

vec3 acesFittedRaw(vec3 color) {
    vec3 a = color * (2.51 * color + 0.03);
    vec3 b = color * (2.43 * color + 0.59) + 0.14;
    return a / max(b, vec3(1e-6));
}

vec3 acesFittedToneMap(vec3 color) {
    return clamp(acesFittedRaw(color), 0.0, 1.0);
}

vec3 acesFittedWhitePointToneMap(vec3 color, float whitePoint) {
    float whiteScale = 1.0 / max(acesFittedRaw(vec3(whitePoint)).r, 1e-6);
    return clamp(acesFittedRaw(color) * whiteScale, 0.0, 1.0);
}

vec3 uncharted2Partial(vec3 x) {
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 uncharted2ToneMap(vec3 color, float whitePoint) {
    vec3 mapped = uncharted2Partial(color);
    float whiteScale = 1.0 / max(uncharted2Partial(vec3(whitePoint)).r, 1e-6);
    return mapped * whiteScale;
}

vec3 applyToneMapping(vec3 color) {
    switch (pc.toneMappingMethod) {
        case 1: return reinhardToneMap(color);
        case 2: return reinhardWhitePointToneMap(color, pc.whitePoint);
        case 3: return acesFittedToneMap(color);
        case 4: return acesFittedWhitePointToneMap(color, pc.whitePoint);
        case 5: return uncharted2ToneMap(color, pc.whitePoint);
        case 0:
        default: return pbrNeutralToneMap(color);
    }
}

vec3 applySaturation(vec3 color, float saturation) {
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(luma), color, saturation);
}

vec3 srgbToLinear(vec3 color) {
    bvec3 cutoff = lessThanEqual(color, vec3(0.04045));
    vec3 low = color / 12.92;
    vec3 high = pow((color + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, cutoff);
}

float cross2(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

bool fireTriangleCoordinates(vec2 point, vec2 a, vec2 b, vec2 c, out vec3 barycentric) {
    float area = cross2(b - a, c - a);
    if (abs(area) < 1e-8) return false;
    barycentric.x = cross2(b - point, c - point) / area;
    barycentric.y = cross2(c - point, a - point) / area;
    barycentric.z = 1.0 - barycentric.x - barycentric.y;
    return all(greaterThanEqual(barycentric, vec3(-1e-5)));
}

vec3 rotateFireVertexY(vec3 vertex, float angle) {
    float sine = sin(angle);
    float cosine = cos(angle);
    return vec3(cosine * vertex.x + sine * vertex.z,
                vertex.y,
                -sine * vertex.x + cosine * vertex.z);
}

vec4 sampleFirePanel(vec2 screenUV, int panel) {
    float side = float(panel * 2 - 1);
    vec3 translation = vec3(-side * 0.24, -0.3, 0.0);
    float angle = radians(side * 10.0);

    vec3 localPosition[4] = vec3[4](
        vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5),
        vec3(0.5, 0.5, -0.5), vec3(-0.5, 0.5, -0.5));
    vec2 localUV[4] = vec2[4](
        vec2(1.0, 1.0), vec2(0.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0));
    vec4 clip[4];
    vec2 projected[4];
    for (int vertex = 0; vertex < 4; ++vertex) {
        clip[vertex] = worldUBO.cameraProjMat
            * vec4(rotateFireVertexY(localPosition[vertex], angle) + translation, 1.0);
        if (abs(clip[vertex].w) < 1e-6) return vec4(0.0);
        projected[vertex] = clip[vertex].xy / clip[vertex].w * 0.5 + 0.5;
    }

    vec3 barycentric;
    ivec3 indices;
    if (fireTriangleCoordinates(screenUV, projected[0], projected[1], projected[2], barycentric)) {
        indices = ivec3(0, 1, 2);
    } else if (fireTriangleCoordinates(screenUV, projected[0], projected[2], projected[3], barycentric)) {
        indices = ivec3(0, 2, 3);
    } else {
        return vec4(0.0);
    }

    vec3 inverseW = vec3(1.0 / clip[indices.x].w,
                         1.0 / clip[indices.y].w,
                         1.0 / clip[indices.z].w);
    vec3 weights = barycentric * inverseW;
    float weightSum = weights.x + weights.y + weights.z;
    if (abs(weightSum) < 1e-8) return vec4(0.0);
    vec2 local = (weights.x * localUV[indices.x]
                + weights.y * localUV[indices.y]
                + weights.z * localUV[indices.z]) / weightSum;
    vec2 atlasUV = vec2(mix(worldUBO.cameraFireUV.x, worldUBO.cameraFireUV.z, local.x),
                        mix(worldUBO.cameraFireUV.y, worldUBO.cameraFireUV.w, local.y));
    return texture(textures[nonuniformEXT(worldUBO.cameraEffectTextureIDs.z)], atlasUV);
}

vec3 applyHdrCameraEffects(vec3 hdr, vec2 screenUV) {
    // Vanilla draws the camera-inside-block quad first and replaces the covered scene color.
    if (worldUBO.cameraEffectTextureIDs.x >= 0) {
        vec2 atlasUV = vec2(mix(worldUBO.cameraBlockUV.z, worldUBO.cameraBlockUV.x, screenUV.x),
                            mix(worldUBO.cameraBlockUV.y, worldUBO.cameraBlockUV.w, screenUV.y));
        vec3 block = srgbToLinear(texture(
            textures[nonuniformEXT(worldUBO.cameraEffectTextureIDs.x)], atlasUV).rgb);
        hdr = block * max(worldUBO.cameraBlockColor.rgb, vec3(0.0));
    }

    // The repeating texture remains a camera-surface cue.  Fog and distance attenuation are
    // already handled by the ray-traced camera medium, so this is only source-over texture color.
    if (worldUBO.cameraEffectTextureIDs.y >= 0) {
        float repeatU = worldUBO.cameraFluidParams.z;
        float repeatV = worldUBO.cameraFluidColor.a;
        vec2 fluidUV = vec2((1.0 - screenUV.x) * repeatU + worldUBO.cameraFluidParams.x,
                            screenUV.y * repeatV + worldUBO.cameraFluidParams.y);
        vec4 fluid = texture(textures[nonuniformEXT(worldUBO.cameraEffectTextureIDs.y)], fluidUV);
        float alpha = clamp(fluid.a * worldUBO.cameraFluidParams.w, 0.0, 1.0);
        vec3 source = srgbToLinear(fluid.rgb) * max(worldUBO.cameraFluidColor.rgb, vec3(0.0));
        hdr = mix(hdr, source, alpha);
    }

    // Fire is a first-person status effect. Keep it in the same pre-exposure baseline as the
    // other camera layers until the HDR route has been verified in a real frame.
    if (worldUBO.cameraEffectTextureIDs.z >= 0) {
        vec4 left = sampleFirePanel(screenUV, 0);
        vec4 right = sampleFirePanel(screenUV, 1);
        float leftAlpha = clamp(left.a * worldUBO.cameraFireParams.x, 0.0, 1.0);
        float rightAlpha = clamp(right.a * worldUBO.cameraFireParams.x, 0.0, 1.0);
        vec3 leftSource = srgbToLinear(left.rgb) * worldUBO.cameraFireParams.y;
        vec3 rightSource = srgbToLinear(right.rgb) * worldUBO.cameraFireParams.y;
        hdr = mix(hdr, leftSource, leftAlpha);
        hdr = mix(hdr, rightSource, rightAlpha);
    }
    return hdr;
}

void main() {
    vec3 hdr = texture(HDR, texCoord).rgb;

    float exposure = (pc.autoExposure != 0) ? expData.exposure : pc.manualExposure;
    if (isnan(exposure) || isinf(exposure) || exposure <= 0.0) { exposure = max(pc.manualExposure, 1e-6); }
    exposure *= exp2(pc.exposureBias);

    hdr = applyHdrCameraEffects(hdr, texCoord);
    vec3 expColor = max(hdr * max(exposure, 0.0), vec3(0.0));
    vec3 mapped = applyToneMapping(expColor);
    mapped = max(mapped, vec3(0.0));
    mapped = applySaturation(mapped, max(pc.saturation, 0.0));
    mapped = pow(mapped, vec3(1.0 / 2.2));
    if (pc.clampOutput != 0) mapped = clamp(mapped, vec3(0.0), vec3(1.0));

    // The composed UI target uses alpha as GUI coverage for DLSS-G. World color
    // starts with zero coverage; subsequent UI draws accumulate real alpha.
    fragColor = vec4(mapped, 0.0);
}
