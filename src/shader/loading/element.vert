#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 colour;
layout(location=0) out vec2 texCoord;
layout(location=1) out vec4 vertexColour;
layout(push_constant) uniform Parameters { vec2 screenSize; int role; float opacity; } params;
void main() {
    gl_Position = vec4((position / params.screenSize) * 2.0 - 1.0, 0.0, 1.0);
    texCoord = uv;
    vertexColour = colour;
}
