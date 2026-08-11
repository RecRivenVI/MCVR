#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#include "util/priority_payload.glsl"
#include "util/priority_coverage.glsl"
#include "common/shared.hpp"

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 1, binding = 1) readonly buffer BLASOffsets { uint offsets[]; } blasOffsets;
layout(set = 1, binding = 2) readonly buffer IndexBufferAddr { uint64_t addrs[]; } indexBufferAddrs;
layout(std430, buffer_reference, buffer_reference_align = 8) readonly buffer IndexBuffer { uint indices[]; } indexBuffer;
#include "util/vertex.glsl"

layout(location = 0) rayPayloadInEXT PriorityRayPayload priorityRay;
hitAttributeEXT vec2 attribs;

void main() {
    uint geometryBufferIndex = getGeometryBufferIndex(gl_InstanceCustomIndexEXT, gl_GeometryIndexEXT);
    uint i0, i1, i2;
    MaterialVertex m0, m1, m2;
    loadTriangleIndices(geometryBufferIndex, gl_PrimitiveID, i0, i1, i2);
    loadTriangleMaterial(geometryBufferIndex, i0, i1, i2, m0, m1, m2);
    vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    vec4 layer = hasColorLayer(m0.packedData)
        ? bary.x * m0.colorLayer + bary.y * m1.colorLayer + bary.z * m2.colorLayer
        : vec4(1.0);
    vec4 texel = vec4(1.0);
    if (hasTexture(m0.packedData)) {
        vec2 uv = bary.x * m0.textureUV + bary.y * m1.textureUV + bary.z * m2.textureUV;
        texel = textureLod(textures[nonuniformEXT(m0.textureID)], uv, 0.0);
    }
    vec4 resolved = resolveTextTextureColor(texel, hasTexture(m0.packedData),
                                            getAlphaMode(m0.packedData));
    float alpha = resolvePriorityAlpha(resolved.a * layer.a, getAlphaMode(m0.packedData));
    ivec2 lightUv = ivec2(round(bary.x * vec2(m0.lightUV) + bary.y * vec2(m1.lightUV)
        + bary.z * vec2(m2.lightUV)));
    float light = hasLight(m0.packedData)
        ? clamp(float(max(lightUv.x, lightUv.y)) / 240.0, 0.0, 1.0)
        : 1.0;
    float emission = bary.x * m0.albedoEmission + bary.y * m1.albedoEmission
        + bary.z * m2.albedoEmission;
    priorityRay.color = vec4(resolved.rgb * layer.rgb * max(light, emission), alpha);
    priorityRay.hitT = gl_HitTEXT;
}
