#ifndef ALPHA_MODE_GLSL
#define ALPHA_MODE_GLSL

const uint ALPHA_MODE_OPAQUE = 0u;
const uint ALPHA_MODE_CUTOUT = 1u;
const uint ALPHA_MODE_TRANSMISSION = 2u;
const uint ALPHA_MODE_CUTOUT_LOW = 9u;
const uint ALPHA_MODE_COVERAGE = 10u;
const uint ALPHA_MODE_ADDITIVE = 11u;
// 12..19 are Radiance's text modes. Flywheel-only material modes use the
// remaining values in the packed five-bit alpha field.
const uint ALPHA_MODE_FLYWHEEL_LIGHTNING = 20u;
const uint ALPHA_MODE_FLYWHEEL_GLINT = 21u;
const uint ALPHA_MODE_FLYWHEEL_CRUMBLING = 22u;
const uint ALPHA_MODE_FLYWHEEL_TRANSLUCENT = 23u;
// Unlit opaque overlay stroke (Catnip outline_solid): written by transparent_only.rchit and
// excluded from shadow/reflection rays by the primary-only instance mask.
const uint ALPHA_MODE_ORDERED_OPAQUE = 24u;

// Kept as a source-level alias for external shader packs compiled against the old name.
const uint ALPHA_MODE_TRANSPARENT = ALPHA_MODE_TRANSMISSION;

const float CUTOUT_ALPHA_THRESHOLD = 0.5;
const float CUTOUT_LOW_ALPHA_THRESHOLD = 0.1;

bool isCutoutAlphaMode(uint alphaMode) {
    return alphaMode == ALPHA_MODE_CUTOUT || alphaMode == ALPHA_MODE_CUTOUT_LOW;
}

bool isTransmissionAlphaMode(uint alphaMode) {
    return alphaMode == ALPHA_MODE_TRANSMISSION;
}

bool isCoverageAlphaMode(uint alphaMode) {
    return alphaMode == ALPHA_MODE_COVERAGE;
}

bool isAdditiveAlphaMode(uint alphaMode) {
    return alphaMode == ALPHA_MODE_ADDITIVE || alphaMode == ALPHA_MODE_FLYWHEEL_LIGHTNING
        || alphaMode == ALPHA_MODE_FLYWHEEL_GLINT;
}

bool isFlywheelLayerAlphaMode(uint alphaMode) {
    return alphaMode == ALPHA_MODE_ADDITIVE || (alphaMode >= ALPHA_MODE_FLYWHEEL_LIGHTNING
        && alphaMode <= ALPHA_MODE_FLYWHEEL_TRANSLUCENT);
}

float resolveSurfaceAlpha(float alpha, uint alphaMode) {
    alpha = clamp(alpha, 0.0, 1.0);

    if (alphaMode == ALPHA_MODE_OPAQUE) { return 1.0; }

    if (alphaMode == ALPHA_MODE_CUTOUT) {
        return alpha >= CUTOUT_ALPHA_THRESHOLD ? 1.0 : 0.0;
    }

    if (alphaMode == ALPHA_MODE_CUTOUT_LOW) {
        return alpha >= CUTOUT_LOW_ALPHA_THRESHOLD ? 1.0 : 0.0;
    }

    return alpha;
}

#endif
