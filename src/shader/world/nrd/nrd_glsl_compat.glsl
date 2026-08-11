// GLSL adapter for the subset of NVIDIA NRD 4.17.3's public shader contract
// used by MCVR. The upstream HLSL-compatible header contains scalar broadcast
// expressions that glslang rejects even when NRD_GLSL is enabled, so keep the
// exact used formulas here instead of compiling the unrelated full header.

#define NRD_NORMAL_ENCODING_RGBA8_UNORM 0
#define NRD_NORMAL_ENCODING_RGBA8_SNORM 1
#define NRD_NORMAL_ENCODING_R10G10B10A2_UNORM 2
#define NRD_NORMAL_ENCODING_RGBA16_UNORM 3
#define NRD_NORMAL_ENCODING_RGBA16_SNORM 4

#define NRD_ROUGHNESS_ENCODING_SQ_LINEAR 0
#define NRD_ROUGHNESS_ENCODING_LINEAR 1
#define NRD_ROUGHNESS_ENCODING_SQRT_LINEAR 2

#define NRD_FP16_MAX 65504.0
#define NRD_EPS 1e-6

vec3 radianceNrdEncodeNormalRoughness101010(vec3 normal, float roughness) {
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);

    vec3 packed;
    packed.y = normal.y * 0.5 + 0.5;
    packed.x = normal.x * 0.5 + packed.y;
    packed.y -= normal.x * 0.5;

    roughness = max(roughness, 1.5 / 512.0);
    float signedRoughness = normal.z < 0.0 ? -roughness : roughness;
    packed.z = signedRoughness * 0.5 + 0.5;
    return packed;
}

vec3 radianceNrdLinearToYCoCg(vec3 color) {
    return vec3(
        dot(color, vec3(0.25, 0.5, 0.25)),
        dot(color, vec3(0.5, 0.0, -0.5)),
        dot(color, vec3(-0.25, 0.5, -0.25)));
}

vec3 radianceNrdYCoCgToLinear(vec3 color) {
    float t = color.x - color.z;
    return max(vec3(t + color.y, color.x + color.z, t - color.y), vec3(0.0));
}

float radianceNrdSpecMagicCurve(float roughness, float power) {
    float factor = 1.0 - exp2(-200.0 * roughness * roughness);
    return factor * pow(clamp(roughness, 0.0, 1.0), power);
}

vec4 NRD_FrontEnd_PackNormalAndRoughness(vec3 normal, float roughness, float materialId) {
#if NRD_ROUGHNESS_ENCODING == NRD_ROUGHNESS_ENCODING_SQRT_LINEAR
    roughness = sqrt(clamp(roughness, 0.0, 1.0));
#elif NRD_ROUGHNESS_ENCODING == NRD_ROUGHNESS_ENCODING_SQ_LINEAR
    roughness *= roughness;
#endif

#if NRD_NORMAL_ENCODING == NRD_NORMAL_ENCODING_R10G10B10A2_UNORM
    return vec4(
        radianceNrdEncodeNormalRoughness101010(normal, roughness),
        clamp(materialId / 3.0, 0.0, 1.0));
#else
    normal /= max(abs(normal.x), max(abs(normal.y), abs(normal.z)));
#if NRD_NORMAL_ENCODING == NRD_NORMAL_ENCODING_RGBA8_UNORM || NRD_NORMAL_ENCODING == NRD_NORMAL_ENCODING_RGBA16_UNORM
    normal = normal * 0.5 + 0.5;
#endif
    return vec4(normal, roughness);
#endif
}

float NRD_FrontEnd_TrimHitDistance(float hitDistance, float threshold) {
    return hitDistance < threshold ? 0.0 : hitDistance;
}

float REBLUR_FrontEnd_GetNormHitDist(
        float hitDistance, float viewZ, vec3 hitDistanceParameters, float roughness) {
    float specMagicCurve = radianceNrdSpecMagicCurve(roughness, 0.5);
    float normalization =
        (hitDistanceParameters.x + abs(viewZ) * hitDistanceParameters.y) *
        mix(hitDistanceParameters.z, 1.0, specMagicCurve);
    return max(clamp(hitDistance / normalization, 0.0, 1.0), NRD_EPS);
}

vec4 REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        vec3 radiance, float normalizedHitDistance, bool sanitize) {
    if (sanitize) {
        radiance = any(isnan(radiance)) || any(isinf(radiance))
            ? vec3(0.0)
            : clamp(radiance, vec3(0.0), vec3(NRD_FP16_MAX));
        normalizedHitDistance = isnan(normalizedHitDistance) || isinf(normalizedHitDistance)
            ? 0.0
            : clamp(normalizedHitDistance, 0.0, 1.0);
    }
    return vec4(radianceNrdLinearToYCoCg(radiance), normalizedHitDistance);
}

vec4 REBLUR_BackEnd_UnpackRadianceAndNormHitDist(vec4 data) {
    data.xyz = radianceNrdYCoCgToLinear(data.xyz);
    return data;
}
