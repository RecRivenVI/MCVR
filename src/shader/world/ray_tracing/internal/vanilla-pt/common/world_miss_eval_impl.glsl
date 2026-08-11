#include "common/shared.hpp"
#include "util/ray.glsl"
#include "util/util.glsl"

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 5, binding = 0) uniform sampler2D transLUT;
layout(set = 5, binding = 2) uniform samplerCube skyFull;

layout(set = 2, binding = 0) uniform WorldUniform {
    WorldUBO worldUBO;
};

layout(set = 2, binding = 1) uniform LastWorldUniform {
    WorldUBO lastWorldUbo;
};

layout(set = 2, binding = 2) uniform SkyUniform {
    SkyUBO skyUBO;
};

layout(location = 0) rayPayloadInEXT MainRay mainRay;

#include "common/volumetric_cloud.glsl"

bool missIntersectSphere(vec3 rayOrigin, vec3 rayDir, float radius, out float tNear, out float tFar) {
    float b = dot(rayOrigin, rayDir);
    float c = dot(rayOrigin, rayOrigin) - radius * radius;
    float h = b * b - c;
    if (h < 0.0) return false;
    h = sqrt(h);
    tNear = -b - h;
    tFar = -b + h;
    return true;
}

void makeBasis(in vec3 n, out vec3 t, out vec3 b) {
    float s = (n.z >= 0.0) ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float k = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * k, -s * n.x);
    b = vec3(k, s + n.y * n.y * a, -n.y);
    t = normalize(t);
    b = normalize(b);
}

vec4 sampleTextureLod0(sampler2D tex, vec2 uv) {
    ivec2 texSize = textureSize(tex, 0);
    if (texSize.x <= 0 || texSize.y <= 0) return vec4(0.0);
    vec2 halfTexel = 0.5 / vec2(texSize);
    vec2 clampedUv = clamp(uv, halfTexel, vec2(1.0) - halfTexel);
    return sampleTexture(tex, clampedUv, 0.0, false);
}

vec4 sampleAtlasLod0(sampler2D tex, vec2 uv01, uvec2 tileCount, uvec2 tile) {
    ivec2 texSize = textureSize(tex, 0);
    if (texSize.x <= 0 || texSize.y <= 0) return vec4(0.0);

    vec2 invTileCount = 1.0 / vec2(tileCount);
    vec2 tileMin = vec2(tile) * invTileCount;
    vec2 tileMax = tileMin + invTileCount;
    vec2 halfTexel = 0.5 / vec2(texSize);
    vec2 minUv = tileMin + halfTexel;
    vec2 maxUv = tileMax - halfTexel;
    vec2 atlasUv = mix(minUv, maxUv, clamp(uv01, 0.0, 1.0));
    return sampleTexture(tex, atlasUv, 0.0, false);
}

vec4 evalSunBillboard(vec3 rayDir) {
    vec3 sunDir = celestialSunDirection();
    rayDir = normalize(rayDir);
    float z = dot(rayDir, sunDir);
    if (z <= 0.0) return vec4(0.0);

    vec3 right, up;
    makeBasis(sunDir, right, up);
    vec2 p = vec2(dot(rayDir, right), dot(rayDir, up));
    vec2 q = p / max(z, 1e-4);
    float tanHalf = tan(0.03);
    vec2 a = abs(q);
    if (a.x > tanHalf || a.y > tanHalf) return vec4(0.0);
    vec2 uv = q / tanHalf * 0.5 + 0.5;
    return sampleTextureLod0(textures[nonuniformEXT(skyUBO.sunTextureID)], uv);
}

vec4 evalMoonBillboard(vec3 rayDir) {
    vec3 moonDir = celestialMoonDirection();
    rayDir = normalize(rayDir);
    float z = dot(rayDir, moonDir);
    if (z <= 0.0) return vec4(0.0);

    vec3 right, up;
    makeBasis(moonDir, right, up);
    vec2 p = vec2(dot(rayDir, right), dot(rayDir, up));
    vec2 q = p / max(z, 1e-4);
    float tanHalf = tan(0.05);
    vec2 a = abs(q);
    if (a.x > tanHalf || a.y > tanHalf) return vec4(0.0);
    vec2 uv = q / tanHalf * 0.5 + 0.5;
    uvec2 tileCount = uvec2(4u, 2u);
    uvec2 tile = uvec2(skyUBO.moonPhase % tileCount.x, (skyUBO.moonPhase / tileCount.x) % tileCount.y);
    return sampleAtlasLod0(textures[nonuniformEXT(skyUBO.moonTextureID)], uv, tileCount, tile);
}

// The vanilla End sky is a six-face cube made by drawing the same 16x16
// end_sky texture on each face with UVs 0..16.  Keep the face rotations from
// LevelRenderer#renderEndSky instead of treating it as the normal atmosphere.
vec3 evalEndSky(vec3 rayDir) {
    rayDir = normalize(rayDir);
    float ax = abs(rayDir.x);
    float ay = abs(rayDir.y);
    float az = abs(rayDir.z);
    vec2 faceUv;

    if (ay >= ax && ay >= az) {
        float d = max(ay, 1e-5);
        faceUv = rayDir.y > 0.0
            ? vec2(rayDir.x / d, -rayDir.z / d)
            : vec2(rayDir.x / d, rayDir.z / d);
    } else if (az >= ax) {
        float d = max(az, 1e-5);
        faceUv = rayDir.z > 0.0
            ? vec2(rayDir.x / d, rayDir.y / d)
            : vec2(rayDir.x / d, -rayDir.y / d);
    } else {
        float d = max(ax, 1e-5);
        faceUv = rayDir.x > 0.0
            ? vec2(rayDir.y / d, rayDir.z / d)
            : vec2(-rayDir.y / d, rayDir.z / d);
    }

    vec2 uv = fract((faceUv * 0.5 + 0.5) * 16.0);
    vec3 textureColor = texture(textures[nonuniformEXT(worldUBO.endSkyTextureID)], uv).rgb;
    // renderEndSky sets the vertex color to ARGB 0xff282828.
    return textureColor * (40.0 / 255.0);
}

vec3 applySunriseGradient(vec3 rayDir, vec3 background) {
    float alpha = clamp(skyUBO.horizonColor.a, 0.0, 1.0);
    if (alpha <= 1e-5) return background;

    vec3 sunHorizon = vec3(skyUBO.sunDirection.x, 0.0, skyUBO.sunDirection.z);
    float sunHorizonLength = length(sunHorizon);
    vec3 rayHorizon = vec3(rayDir.x, 0.0, rayDir.z);
    float rayHorizonLength = length(rayHorizon);
    if (sunHorizonLength <= 1e-5 || rayHorizonLength <= 1e-5) return background;

    float azimuth = dot(rayHorizon / rayHorizonLength, sunHorizon / sunHorizonLength);
    // The vanilla triangle fan is centered on the sunrise/sunset azimuth and
    // fades toward both the vertical and the edge of its 120-block disc.
    float azimuthWeight = smoothstep(0.10, 0.92, azimuth);
    float verticalWeight = 1.0 - smoothstep(0.0, 0.65, abs(rayDir.y));
    float blend = clamp(alpha * azimuthWeight * verticalWeight, 0.0, 1.0);
    return mix(background, skyUBO.horizonColor.rgb, blend);
}

uint starHash(uvec2 value) {
    uint h = value.x * 0x8da6b343u ^ value.y * 0xd8163841u ^ 0xcb1ab31fu;
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    return h ^ (h >> 16u);
}

vec3 evalPathTracedStars(vec3 rayDir) {
    const uvec2 gridSize = uvec2(512u, 256u);
    vec2 sphericalUv = vec2(atan(rayDir.z, rayDir.x) * INV_TWO_PI + 0.5,
                            asin(clamp(rayDir.y, -1.0, 1.0)) * INV_PI + 0.5);
    uvec2 cell = uvec2(floor(fract(sphericalUv) * vec2(gridSize)));
    uint h = starHash(cell);
    // Approximately 3000 stable stars over the full environment sphere.
    if ((h % 44u) != 0u) { return vec3(0.0); }

    vec2 jitter = vec2(float((h >> 8u) & 0xffffu), float((h >> 16u) & 0xffffu)) / 65535.0;
    vec2 centerUv = (vec2(cell) + 0.15 + jitter * 0.70) / vec2(gridSize);
    float longitude = (centerUv.x - 0.5) * TWO_PI;
    float latitude = (centerUv.y - 0.5) * PI;
    vec3 centerDir = vec3(cos(latitude) * cos(longitude), sin(latitude),
                          cos(latitude) * sin(longitude));
    float angularDistance = acos(clamp(dot(normalize(rayDir), centerDir), -1.0, 1.0));
    float radius = mix(0.00065, 0.00125, float(h & 0xffu) / 255.0);
    float coverage = 1.0 - smoothstep(radius * 0.35, radius, angularDistance);
    return vec3(coverage * 1.8);
}

void main() {
    mainRay.directLightRadiance.x = 1.0;

    if (skyUBO.cameraSubmersionType == 0 || skyUBO.cameraSubmersionType == 2 || skyUBO.hasBlindnessOrDarkness > 0) {
        raySetStop(mainRay, true);
        mainRay.hitT = INF_DISTANCE;
        return;
    }

    if (worldUBO.skyType == 0) {
        raySetStop(mainRay, true);
        mainRay.hitT = INF_DISTANCE;
        return;
    }

    vec3 rayDir = normalize(gl_WorldRayDirectionEXT);
    if (skyUBO.isSkyDark > 0 && worldUBO.skyType == 1 && rayDir.y <= 0.0) {
        raySetStop(mainRay, true);
        mainRay.hitT = INF_DISTANCE;
        return;
    }
    vec3 backgroundRadiance;
    float progress = clamp(skyUBO.rainGradient, 0.0, 1.0);

    if (worldUBO.skyType == 2) {
        backgroundRadiance = evalEndSky(rayDir);
    } else {
        vec3 sunDir = celestialSunDirection();
        vec3 rainyRadiance = mix(vec3(0.0), vec3(0.1), smoothstep(-0.3, 0.3, sunDir.y));
        backgroundRadiance = mix(texture(skyFull, rayDir).rgb, rainyRadiance, progress);
        backgroundRadiance = applySunriseGradient(rayDir, backgroundRadiance);
    }

    if (worldUBO.skyType == 1) {
        float cameraHeight = worldUBO.cameraViewMatInv[3].y;
        vec3 pPlanet = vec3(0.0, VPT_ATMOSPHERE_RG + cameraHeight + 70.0, 0.0);
        float r = clamp(length(pPlanet), VPT_ATMOSPHERE_RG, VPT_ATMOSPHERE_RT);
        vec3 up = pPlanet / max(r, 1e-6);
        float mu = clamp(dot(up, rayDir), -1.0, 1.0);
        vec3 transmittance = sampleCloudAtmosphereTransmittance(r, mu);

        vec4 sunSample = evalSunBillboard(rayDir);
        if (sunSample.a > 1e-4) {
            float tG0, tG1;
            bool hitGround = missIntersectSphere(pPlanet, rayDir, VPT_ATMOSPHERE_RG, tG0, tG1);
            bool blocked = hitGround && (tG1 > 1e-3);
            if (!blocked) {
                vec3 sunRadiance = sunSample.rgb * VPT_SUN_RADIANCE * transmittance * sunSample.a;
                backgroundRadiance += mix(sunRadiance, vec3(0.0), progress);
            }
        }

        vec4 moonSample = evalMoonBillboard(rayDir);
        if (moonSample.a > 1e-4) {
            float tG0, tG1;
            bool hitGround = missIntersectSphere(pPlanet, rayDir, VPT_ATMOSPHERE_RG, tG0, tG1);
            bool blocked = hitGround && (tG1 > 1e-3);
            if (!blocked) {
                vec3 moonRadiance = moonSample.rgb * VPT_MOON_RADIANCE * max(transmittance, vec3(0.03));
                backgroundRadiance += mix(moonRadiance, vec3(0.0), progress);
            }
        }

        backgroundRadiance += evalPathTracedStars(rayDir) * clamp(skyUBO.starBrightness, 0.0, 1.0);
    }

#if VPT_ALLOW_VOLUMETRIC_CLOUD_MISS
    if (worldUBO.skyType == 1 && VPT_CLOUD_MODE == 2u) {
        VolumetricCloudResult cloudResult =
            rayUseIndirectVolumetricCloud(mainRay) ?
                applyVolumetricCloudBudgeted(gl_WorldRayOriginEXT, rayDir, backgroundRadiance,
                                             VPT_INDIRECT_VOLUMETRIC_CLOUD_VIEW_STEPS,
                                             VPT_INDIRECT_VOLUMETRIC_CLOUD_LIGHT_STEPS,
                                             VPT_INDIRECT_VOLUMETRIC_CLOUD_AMBIENT_STEPS) :
                applyVolumetricCloud(gl_WorldRayOriginEXT, rayDir, backgroundRadiance);
        backgroundRadiance = cloudResult.color;
        vec3 clampedBackground = max(backgroundRadiance, vec3(0.0));
        vec3 cloudOnly = max(cloudResult.color - clampedBackground * cloudResult.transmittance, vec3(0.0));
        float backgroundLum = volumetricCloudLuminance(clampedBackground);
        float cloudLum = volumetricCloudLuminance(cloudOnly);
        float cloudPresence = cloudLum / max(backgroundLum + cloudLum, 1e-4);
        float starVisibility = min(cloudResult.transmittance, 1.0 - cloudPresence * 1.35);
        mainRay.directLightRadiance.x = cloudSaturate(starVisibility);
    }
#endif

    mainRay.radiance += backgroundRadiance * mainRay.throughput;
    raySetStop(mainRay, true);
    mainRay.hitT = INF_DISTANCE;
}
