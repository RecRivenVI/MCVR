#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(location=0) in vec2 texCoord;
layout(location=1) in vec4 vertexColour;
layout(location=0) out vec4 fragmentColour;
layout(push_constant) uniform Parameters { vec2 screenSize; int role; float opacity; } params;
void main() {
    if (params.role == 0)
        fragmentColour = vec4(1.0, 1.0, 1.0, texture(image, texCoord).r) * vertexColour;
    else if (params.role == 1)
        fragmentColour = texture(image, texCoord) * vertexColour;
    else
        fragmentColour = vertexColour;
    fragmentColour.a *= params.opacity;
}
