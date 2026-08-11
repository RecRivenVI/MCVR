#ifndef VERTEX_SHADING_NORMAL_GLSL
#define VERTEX_SHADING_NORMAL_GLSL

const uint VERTEX_SHADING_NORMAL_BIT = 1u << 7u;

bool hasVertexShadingNormal(uint packedData) {
    return (packedData & VERTEX_SHADING_NORMAL_BIT) != 0u;
}

// The ray triangle controls face acceptance and geometric offsets. A marked
// producer may provide a distinct smooth BRDF normal; use the same inverse-
// transpose transform as the geometric normal, then orient it to that face.
vec3 vertexBrdfNormal(uint packedData, vec3 n0, vec3 n1, vec3 n2,
                      vec3 bary, mat3 normalMatrix, vec3 geometricNormal) {
    if (!hasVertexShadingNormal(packedData)) return geometricNormal;
    vec3 source = bary.x * n0 + bary.y * n1 + bary.z * n2;
    if (any(isnan(source)) || any(isinf(source)) || dot(source, source) <= 1e-12)
        return geometricNormal;
    vec3 transformed = normalMatrix * source;
    float lengthSquared = dot(transformed, transformed);
    if (any(isnan(transformed)) || any(isinf(transformed)) ||
        !(lengthSquared > 1e-12) || isinf(lengthSquared)) return geometricNormal;
    vec3 normal = transformed * inversesqrt(lengthSquared);
    if (dot(normal, geometricNormal) < 0.0) normal = -normal;
    return dot(normal, geometricNormal) <= 1e-5 ? geometricNormal : normal;
}

#endif
