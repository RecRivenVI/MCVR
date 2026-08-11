#version 450

#ifndef CLEAR_ATTACHMENTS
#define CLEAR_ATTACHMENTS 1
#endif

layout(location = 0) out vec4 color[CLEAR_ATTACHMENTS];

void main() {
    // CONSTANT_COLOR/CONSTANT_ALPHA blending supplies the clear value. The dynamic
    // component write masks and stencil replace masks preserve untouched channels.
    for (int i = 0; i < CLEAR_ATTACHMENTS; ++i) color[i] = vec4(1.0);
}
