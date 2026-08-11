#ifndef RADIANCE_DIAGRAM_MATH_GLSL
#define RADIANCE_DIAGRAM_MATH_GLSL
// Simulated 1.3.2's 8-column palette and 32x32 dither texture. Keep this
// independent of physical render resolution; only low-resolution upscaling
// was retired, not the producer's fade and palette semantics.
const int bitDepth = 32;
const float contrast = 1.8;
const float luminosityOffset = 0.18;

float ditherThreshold(vec2 logicalPixel) {
    ivec2 pixel = ivec2(floor(logicalPixel)) & ivec2(31);
    int value = 0;
    for (int bit = 0; bit < 5; ++bit) {
        int x = (pixel.x >> bit) & 1;
        int y = (pixel.y >> bit) & 1;
        int digit = y == 0 ? (x == 0 ? 0 : 2) : (x == 0 ? 3 : 1);
        value = value * 4 + digit;
    }
    // Match the actual UNORM8 PNG: its 10-bit Bayer rank is truncated to 8 bits.
    return (float(value >> 2) / 255.0) * 0.99 + 0.005;
}

float quantizedLuminosity(vec3 color, float multiplier) {
    float luminosity = dot(color, vec3(0.299, 0.587, 0.114)) * multiplier;
    luminosity = (luminosity - 0.5 + luminosityOffset) * contrast + 0.5;
    luminosity = clamp(luminosity, 0.0, 1.0);
    return floor(luminosity * float(bitDepth)) / float(bitDepth);
}

vec2 diagramLogicalPixel(vec2 targetPixel, vec4 rect, vec2 logicalSize) {
    vec2 topLeft = (targetPixel - rect.xy) * logicalSize / rect.zw;
    return vec2(topLeft.x, logicalSize.y - topLeft.y);
}

float ditherMask(float value, vec2 logicalPixel) {
    float luminosity = max(quantizedLuminosity(vec3(value), 1.0) - 0.00001, 0.0);
    float scaled = luminosity * 7.0;
    float lower = floor(scaled);
    // Return the selected gradient level, not just the binary Bayer decision.
    return (lower + step(ditherThreshold(logicalPixel), fract(scaled))) / 7.0;
}
#endif
