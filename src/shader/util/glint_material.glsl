#ifndef GLINT_MATERIAL_GLSL
#define GLINT_MATERIAL_GLSL

// Glint is a coating on an existing surface. It may change reflected light and
// add a small colored radiance term, but it never changes base alpha,
// transmission, or shadow throughput.
vec3 applyGlintMaterialLayer(inout LabPBRMat mat, vec3 sampledGlint) {
    float strength = clamp(worldUBO.glintStrength, 0.0, 1.0);
    vec3 glintColor = clamp(sampledGlint * strength, vec3(0.0), vec3(1.0));
    float mask = max(glintColor.r, max(glintColor.g, glintColor.b));
    if (mask <= 1.0e-6) { return vec3(0.0); }

    mat.f0 = max(mat.f0, glintColor * 0.65);
    mat.roughness = mix(mat.roughness, 0.12, mask);
    // This radiance is the final coating, after the base and external PBR material
    // have been resolved. Do not attenuate it a second time here: vanilla already
    // shapes the glint mask by squaring the sampled texture.
    return glintColor;
}

#endif
