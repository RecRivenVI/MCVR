#include "common/shared.hpp"

layout(set = 0, binding = 1) uniform sampler2D frame;
layout(set = 0, binding = 2) uniform sampler2D diagramColor;
layout(set = 0, binding = 3) uniform sampler2D diagramDepth;
layout(std140, set = 1, binding = 1) uniform PostUniform {
    OverlayPostUBO ubo;
};

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

#include "diagram_math.glsl"

const vec3 mainPalette[8] = vec3[](
    vec3(48, 49, 51) / 255.0,
    vec3(57, 58, 60) / 255.0,
    vec3(89, 90, 87) / 255.0,
    vec3(122, 121, 115) / 255.0,
    vec3(154, 152, 142) / 255.0,
    vec3(187, 183, 170) / 255.0,
    vec3(212, 207, 191) / 255.0,
    vec3(225, 220, 203) / 255.0
);

const vec3 notePalette[8] = vec3[](
    vec3(40, 34, 24) / 255.0,
    vec3(63, 56, 40) / 255.0,
    vec3(86, 79, 54) / 255.0,
    vec3(115, 107, 70) / 255.0,
    vec3(146, 136, 85) / 255.0,
    vec3(175, 163, 102) / 255.0,
    vec3(193, 181, 124) / 255.0,
    vec3(211, 199, 145) / 255.0
);

vec3 samplePalette(float u, bool note) {
    float position = clamp(u * 8.0 - 0.5, 0.0, 7.0);
    int lower = int(floor(position));
    int upper = min(lower + 1, 7);
    float amount = fract(position);
    return note ? mix(notePalette[lower], notePalette[upper], amount)
                : mix(mainPalette[lower], mainPalette[upper], amount);
}

vec3 palettedDither(vec3 color, vec2 logicalPixel, bool note) {
    float luminosity = quantizedLuminosity(color, 1.7);
    float colorX = 11.0;
    float texel = 1.0 / colorX;
    luminosity = max(luminosity - 0.00001, 0.0);
    float lower = floor(luminosity * colorX) * texel;
    float upper = (floor(luminosity * colorX) + 1.0) * texel;
    float scaled = luminosity * colorX - floor(luminosity * colorX);
    float samplePosition = mix(lower, upper, step(ditherThreshold(logicalPixel), scaled));
    return samplePalette(samplePosition, note);
}

float sdBox(vec2 p, vec2 b) {
    vec2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

float sceneDistance(vec2 logicalPixel, vec2 logicalSize, float fadeScale) {
    float box = sdBox(logicalPixel - logicalSize / 2.0,
                      logicalSize / vec2(2.3, 2.4) / fadeScale);
    float rotationGizmo = sdBox(logicalPixel - vec2(238.0, 175.0), vec2(25.0, 26.0));
    float buttons = sdBox(logicalPixel - vec2(12.0, 161.0), vec2(25.0, 66.0));
    return max(-min(rotationGizmo, buttons), box);
}

vec4 outlineColor(vec2 uv, vec2 depthStep) {
    float center = texture(diagramDepth, uv).r;
    float left = texture(diagramDepth, uv - vec2(depthStep.x, 0.0)).r;
    float right = texture(diagramDepth, uv + vec2(depthStep.x, 0.0)).r;
    float up = texture(diagramDepth, uv - vec2(0.0, depthStep.y)).r;
    float down = texture(diagramDepth, uv + vec2(0.0, depthStep.y)).r;
    float leftDiff = pow(abs(center - left), 1.04);
    float rightDiff = pow(abs(center - right), 1.04);
    float upDiff = pow(abs(center - up), 1.04);
    float downDiff = pow(abs(center - down), 1.04);
    float alpha = min(clamp(leftDiff + rightDiff + upDiff + downDiff, 0.0, 1.0) * 20.0, 1.0);
    vec4 color = ubo.diagramLineColor;
    if (leftDiff > rightDiff || upDiff > downDiff) color = ubo.diagramShadowColor;
    return vec4(color.rgb, alpha * color.a);
}

void main() {
#ifdef DIAGRAM_TARGET
    vec4 background = vec4(0.0);
#else
    vec4 background = texture(frame, texCoord);
#endif
    vec2 pixel = gl_FragCoord.xy;
    vec4 rect = ubo.diagramRect;
    if (pixel.x < rect.x || pixel.y < rect.y || pixel.x >= rect.x + rect.z || pixel.y >= rect.y + rect.w) {
        fragColor = background;
        return;
    }

    vec2 logicalSize = ubo.diagramParams.zw;
    vec2 logicalPixel = diagramLogicalPixel(pixel, rect, logicalSize);
    vec2 depthStep = (rect.zw / logicalSize) / ubo.inSize * vec2(1.0, -1.0);
    float depth = texture(diagramDepth, texCoord).r;
    vec4 source = texture(diagramColor, texCoord);
    if (depth > 0.99) {
        fragColor = background;
        return;
    }

    bool note = ubo.diagramParams.x >= 0.5;
    vec3 dithered = palettedDither(source.rgb, logicalPixel, note);
    vec4 outline = outlineColor(texCoord, depthStep);
    vec3 diagram = mix(dithered, outline.rgb, outline.a);

    float distance = sceneDistance(logicalPixel, logicalSize, ubo.diagramParams.y);
    float fadeValue = 1.0 - distance * 0.08;
    float alpha = ditherMask(fadeValue, logicalPixel);
#ifdef DIAGRAM_TARGET
    fragColor = vec4(diagram, alpha);
#else
    fragColor = vec4(mix(background.rgb, diagram, alpha),
                     alpha + background.a * (1.0 - alpha));
#endif
}
