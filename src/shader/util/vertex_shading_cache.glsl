#ifndef VERTEX_SHADING_CACHE_GLSL
#define VERTEX_SHADING_CACHE_GLSL

// Bit 7 tags a union within the existing RGBA32F surface-cache layers.
// In that mode the producer has no glint/overlay UV: cache6.x, cache6.w,
// cache7.x hold the pre-normal-map BRDF base normal. cache3 stays geometric.
vec4 encodeVertexShadingAux(bool tagged, vec4 ordinaryCoatings, vec3 sourceNormal) {
    return tagged ? vec4(sourceNormal.x, ordinaryCoatings.yz, sourceNormal.y)
                  : ordinaryCoatings;
}

float encodeVertexShadingOverlayY(bool tagged, float ordinaryOverlayY, vec3 sourceNormal) {
    return tagged ? sourceNormal.z : ordinaryOverlayY;
}

vec3 decodeVertexShadingBase(bool tagged, vec4 cache6, vec4 cache7,
                             vec3 geometricNormal) {
    if (!tagged) return geometricNormal;
    vec3 source = vec3(cache6.x, cache6.w, cache7.x);
    float squared = dot(source, source);
    if (any(isnan(source)) || any(isinf(source)) ||
        !(squared > 1e-12) || isinf(squared)) return geometricNormal;
    // The writer stores the already normalized and hemisphere-tested base.
    // Repeating that threshold after an FP32 cache roundtrip can turn a valid
    // near-tangent source normal into the geometric normal discontinuously.
    return source;
}

#endif
