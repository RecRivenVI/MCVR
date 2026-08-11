#version 460
layout(location = 0) flat in uvec2 data;
layout(location = 0) out vec4 Color;
void main() {
    Color = all(equal(data, uvec2(0x12345679u, 0x89ABCDEFu)))
        ? vec4(0.2, 0.8, 0.4, 1.0)
        : vec4(0.9, 0.1, 0.1, 1.0);
}
