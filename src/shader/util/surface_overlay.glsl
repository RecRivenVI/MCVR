#ifndef RADIANCE_SURFACE_OVERLAY_GLSL
#define RADIANCE_SURFACE_OVERLAY_GLSL
// Surface-albedo recoloring before LabPBR evaluation. Coverage is independent
// of material opacity: spring stress and hurt/white-flash overlays use the
// same operation, while the latter's texture encodes inverse coverage.
vec3 applySurfaceOverlay(vec3 albedo, vec3 overlay, float coverage) {
    return mix(albedo, overlay, clamp(coverage, 0.0, 1.0));
}
#endif
