#ifndef RADIANCE_SURFACE_OVERLAY_GLSL
#define RADIANCE_SURFACE_OVERLAY_GLSL
// Surface-albedo recoloring before LabPBR evaluation. Coverage is independent
// of material opacity: spring stress and hurt/white-flash overlays use the
// same operation, while the latter's texture encodes inverse coverage.
vec3 applySurfaceOverlay(vec3 albedo, vec3 overlay, float coverage) {
    return mix(albedo, overlay, clamp(coverage, 0.0, 1.0));
}

// Preserve the producer's two distinct color operations: ordinary vertex RGB
// multiplies the sampled base, while spring Stress RGB mixes over it. Vanilla
// entity UV1 then selects an overlay texel whose alpha stores inverse coverage.
vec3 applySurfaceMaterialColor(vec3 sampledAlbedo, vec4 colorLayer,
                               bool colorLayerMix, bool useOverlay, vec4 overlayColor) {
    vec3 base = colorLayerMix ?
        applySurfaceOverlay(sampledAlbedo, colorLayer.rgb, colorLayer.a) :
        sampledAlbedo * colorLayer.rgb;
    return useOverlay ?
        applySurfaceOverlay(base, overlayColor.rgb, 1.0 - overlayColor.a) : base;
}
#endif
