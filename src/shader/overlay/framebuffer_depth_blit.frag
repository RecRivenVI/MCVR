#version 450

layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(push_constant) uniform Blit {
    vec4 sourceRect;
    vec4 destinationRect;
    vec2 sourceSize;
    vec2 destinationSize;
    int sourceLevel;
} blit;

void main() {
    vec2 pixel = vec2(gl_FragCoord.x, blit.destinationSize.y - gl_FragCoord.y);
    vec2 t = (pixel - blit.destinationRect.xy) /
        (blit.destinationRect.zw - blit.destinationRect.xy);
    if (any(lessThan(t, vec2(0.0))) || any(greaterThanEqual(t, vec2(1.0)))) discard;
    vec2 source = mix(blit.sourceRect.xy, blit.sourceRect.zw, t);
    if (any(lessThan(source, vec2(0.0))) || any(greaterThanEqual(source, blit.sourceSize))) discard;
    gl_FragDepth = textureLod(sourceImage,
        vec2(source.x, blit.sourceSize.y - source.y) / blit.sourceSize, float(blit.sourceLevel)).r;
}
