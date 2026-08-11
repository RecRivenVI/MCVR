#version 450

layout(set = 0, binding = 0) uniform sampler2D firstHitDepthSampler;

layout(push_constant) uniform MainDepthPushConstant {
    vec4 projection;
    vec2 targetSize;
    vec2 padding;
} pc;

void main() {
    ivec2 sourceSize = textureSize(firstHitDepthSampler, 0);
    vec2 normalized = gl_FragCoord.xy / max(pc.targetSize, vec2(1.0));
    ivec2 sourcePixel = clamp(ivec2(normalized * vec2(sourceSize)), ivec2(0), sourceSize - 1);
    float linearDepth = texelFetch(firstHitDepthSampler, sourcePixel, 0).r;
    if (isnan(linearDepth) || isinf(linearDepth) || linearDepth <= 0.0) {
        gl_FragDepth = 1.0;
        return;
    }

    float viewZ = -linearDepth;
    float clipZ = pc.projection.x * viewZ + pc.projection.y;
    float clipW = pc.projection.z * viewZ + pc.projection.w;
    gl_FragDepth = abs(clipW) > 1e-8 ? clamp(clipZ / clipW, 0.0, 1.0) : 1.0;
}
