#version 460
void main() {
    vec2 p[3] = vec2[3](vec2(-0.96, -0.94), vec2(-0.80, -0.94), vec2(-0.96, -0.72));
    gl_Position = vec4(p[gl_VertexIndex], 0.2, 1.0);
}
