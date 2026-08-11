#version 460

layout(set = 0, binding = 1) uniform sampler2D frame;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 imageSize = vec2(textureSize(frame, 0));
    vec2 mosaicSize = imageSize / 4.0;
    vec2 snappedUv = texCoord - fract(texCoord * mosaicSize) / mosaicSize;
    vec3 source = texture(frame, snappedUv).rgb;

    // Minecraft's color_convolve pass keeps luminance in green, then the bits
    // pass quantizes it. Keep that recognizable intent while executing as one
    // HDR-safe Vulkan pass after the ray-traced world has been composed.
    float green = clamp(dot(source, vec3(0.3, 0.59, 0.11)) * 1.328, 0.0, 1.0);
    green -= fract(green * 16.0) / 16.0;
    green = clamp(green * 1.205, 0.0, 1.0);
    fragColor = vec4(0.0, green, 0.0, texture(frame, snappedUv).a);
}
