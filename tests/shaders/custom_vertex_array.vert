#version 460
layout(location = 0) in vec3 QuadPosition;
layout(location = 1) in vec3 SableNormal;
layout(location = 2) in uvec2 SableData;
layout(location = 0) flat out uvec2 data;
void main() {
    gl_Position = vec4(QuadPosition.xy * 0.75, 0.0, 1.0);
    data = SableData + uvec2(uint(SableNormal.z == 1.0), 0u);
}
