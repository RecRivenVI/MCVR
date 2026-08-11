#ifndef RADIANCE_PRIORITY_COVERAGE_GLSL
#define RADIANCE_PRIORITY_COVERAGE_GLSL
#include "text_mode.glsl"
#include "alpha_mode.glsl"

float resolvePriorityAlpha(float alpha, uint mode) {
    // Text retains fractional coverage. Ordinary surfaces retain their material
    // alpha contract even when they are redrawn without world-depth testing.
    return isTextMode(mode) ? clamp(alpha, 0.0, 1.0) : resolveSurfaceAlpha(alpha, mode);
}
#endif
