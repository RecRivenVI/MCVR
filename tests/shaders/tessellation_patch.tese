#version 460
layout(quads, equal_spacing, ccw) in;
void main() {
    vec4 left = mix(gl_in[0].gl_Position, gl_in[3].gl_Position, gl_TessCoord.y);
    vec4 right = mix(gl_in[1].gl_Position, gl_in[2].gl_Position, gl_TessCoord.y);
    gl_Position = mix(left, right, gl_TessCoord.x);
}
