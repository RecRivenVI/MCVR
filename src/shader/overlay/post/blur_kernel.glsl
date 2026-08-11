#ifndef RADIANCE_BLUR_KERNEL
#define RADIANCE_BLUR_KERNEL
// Shared by the final-color pass and HUD-less processing: identical samples,
// linear filtering, edge treatment and normalization are part of the FG contract.
vec4 radianceBlur(sampler2D source, vec2 uv, vec2 step, float radius) {
    vec4 color = vec4(0);
    for (float a = -radius + 0.5; a <= radius; a += 2.0)
        color += texture(source, uv + step * a);
    color += texture(source, uv + step * radius) * 0.5;
    return color / (radius + 0.5);
}
#endif
