#ifndef EMISSIVE_OVERLAY_GLSL
#define EMISSIVE_OVERLAY_GLSL

vec3 sampleEmissiveOverlay(uint textureID, vec2 uv, float lod) {
    if (textureID == 0u) { return vec3(0.0); }

    vec4 overlay = sampleTexture(textures[nonuniformEXT(textureID)], uv, lod, false);
    return max(overlay.rgb, vec3(0.0)) * clamp(overlay.a, 0.0, 1.0);
}

#endif
