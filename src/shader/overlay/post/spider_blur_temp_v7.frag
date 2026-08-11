#version 460

layout(set = 0, binding = 5) uniform sampler2D sourceImage;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 sampleStep = vec2(0.0, 1.0) / vec2(textureSize(sourceImage, 0));
    vec4 blurred = vec4(0.0);
    const float radius = 7.0;
    for (float a = -radius + 0.5; a <= radius; a += 2.0) {
        blurred += texture(sourceImage, texCoord + sampleStep * a);
    }
    blurred += texture(sourceImage, texCoord + sampleStep * radius) / 2.0;
    fragColor = blurred / (radius + 0.5);
}

