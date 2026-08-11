#version 460

layout(set = 0, binding = 1) uniform sampler2D frame;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec4 source = texture(frame, texCoord);
    fragColor = vec4(mix(source.rgb, vec3(1.0) - source.rgb, 0.8), source.a);
}
