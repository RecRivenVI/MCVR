#ifndef TEXT_MODE_GLSL
#define TEXT_MODE_GLSL

// 0..11 are reserved for regular material alpha modes. Text has its own
// namespace so a world text surface can never become a cutout/transmission
// surface merely because it uses the same packed field.
const uint POST_TEXT_MODE_BACKGROUND = 12u;
const uint POST_TEXT_MODE_INTENSITY = 13u;
const uint POST_TEXT_MODE_RGBA = 14u;
const uint POST_TEXT_MODE_BACKGROUND_SEE_THROUGH = 15u;
const uint POST_TEXT_MODE_INTENSITY_SEE_THROUGH = 16u;
const uint POST_TEXT_MODE_RGBA_SEE_THROUGH = 17u;
const uint POST_TEXT_MODE_INTENSITY_POLYGON_OFFSET = 18u;
const uint POST_TEXT_MODE_RGBA_POLYGON_OFFSET = 19u;

bool isTextBackgroundMode(uint textMode) {
    return textMode == POST_TEXT_MODE_BACKGROUND || textMode == POST_TEXT_MODE_BACKGROUND_SEE_THROUGH;
}

bool isTextIntensityMode(uint textMode) {
    return textMode == POST_TEXT_MODE_INTENSITY || textMode == POST_TEXT_MODE_INTENSITY_SEE_THROUGH ||
           textMode == POST_TEXT_MODE_INTENSITY_POLYGON_OFFSET;
}

bool isTextRgbaMode(uint textMode) {
    return textMode == POST_TEXT_MODE_RGBA || textMode == POST_TEXT_MODE_RGBA_SEE_THROUGH ||
           textMode == POST_TEXT_MODE_RGBA_POLYGON_OFFSET;
}

bool isTextMode(uint textMode) {
    return isTextBackgroundMode(textMode) || isTextIntensityMode(textMode) || isTextRgbaMode(textMode);
}

vec4 resolveTextTextureColor(vec4 textureColor, bool useTexture, uint textMode) {
    if (isTextBackgroundMode(textMode)) { return vec4(1.0); }
    if (isTextIntensityMode(textMode)) { return textureColor.rrrr; }
    if (isTextRgbaMode(textMode)) { return textureColor; }
    return useTexture ? textureColor : vec4(1.0);
}

float resolveTextCoverage(vec4 textureColor, bool useTexture, float colorLayerAlpha, uint textMode) {
    return clamp(resolveTextTextureColor(textureColor, useTexture, textMode).a * colorLayerAlpha, 0.0, 1.0);
}

#endif
