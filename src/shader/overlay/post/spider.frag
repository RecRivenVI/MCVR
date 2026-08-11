#version 460

layout(set = 0, binding = 1) uniform sampler2D mainImage;
layout(set = 0, binding = 2) uniform sampler2D largeBlurImage;
layout(set = 0, binding = 4) uniform sampler2D smallBlurImage;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

vec2 scaledCoord(vec2 uv, vec2 scale, vec2 offset, float rotationDegrees) {
    float angle = radians(rotationDegrees);
    float sine = sin(angle);
    float cosine = cos(angle);
    vec2 rotated = vec2(uv.x * cosine - uv.y * sine,
                        uv.y * cosine + uv.x * sine);
    return rotated * scale + offset;
}

vec4 spiderClip(vec4 diffuseTexel, vec4 previous, vec2 coord,
                vec4 scissor, vec4 vignette) {
    vec4 result = diffuseTexel;
    if (coord.x < scissor.x) result = previous;
    if (coord.y < scissor.y) result = previous;
    if (coord.x > scissor.z) result = previous;
    if (coord.y > scissor.w) result = previous;

    if (coord.x < vignette.x) {
        result = mix(previous, result,
                     (scissor.x - coord.x) / (scissor.x - vignette.x));
    }
    if (coord.y < vignette.y) {
        result = mix(previous, result,
                     (scissor.y - coord.y) / (scissor.y - vignette.y));
    }
    if (coord.x > vignette.z) {
        result = mix(previous, result,
                     (scissor.z - coord.x) / (scissor.z - vignette.z));
    }
    if (coord.y > vignette.w) {
        result = mix(previous, result,
                     (scissor.w - coord.y) / (scissor.w - vignette.w));
    }
    return result;
}

void main() {
    vec2 eye1 = scaledCoord(texCoord, vec2(1.25, 2.0), vec2(-0.125, -0.1), 0.0);
    vec4 result = spiderClip(texture(mainImage, eye1),
                             texture(largeBlurImage, texCoord), eye1,
                             vec4(0.0, 0.0, 1.0, 1.0),
                             vec4(0.1, 0.1, 0.9, 0.9));

    vec2 eye2 = scaledCoord(texCoord, vec2(2.35, 4.2), vec2(-1.1, -1.5), -45.0);
    result = spiderClip(texture(smallBlurImage, eye2), result, eye2,
                        vec4(0.21, 0.0, 0.79, 1.0),
                        vec4(0.31, 0.1, 0.69, 0.9));

    vec2 eye3 = scaledCoord(texCoord, vec2(2.35, 4.2), vec2(0.45, -4.45), 45.0);
    result = spiderClip(texture(smallBlurImage, eye3), result, eye3,
                        vec4(0.21, 0.0, 0.79, 1.0),
                        vec4(0.31, 0.1, 0.69, 0.9));

    vec2 eye4 = scaledCoord(texCoord, vec2(2.35), vec2(-0.385, -1.29), 0.0);
    result = spiderClip(texture(smallBlurImage, eye4), result, eye4,
                        vec4(0.0, 0.0, 1.0, 1.0),
                        vec4(0.31, 0.1, 0.69, 0.9));

    vec2 eye5 = scaledCoord(texCoord, vec2(2.35), vec2(-0.965, -1.29), 0.0);
    result = spiderClip(texture(smallBlurImage, eye5), result, eye5,
                        vec4(0.0, 0.0, 1.0, 1.0),
                        vec4(0.31, 0.1, 0.69, 0.9));

    fragColor = vec4(result.rgb * vec3(1.0, 0.8, 0.8), result.a);
}

