#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace mcvr::instancing {
inline glm::mat4 instanceWorldTransform(glm::ivec3 origin, const glm::mat4 &embedding,
                                        const glm::mat4 &local) {
    return glm::translate(glm::mat4(1), glm::vec3(origin)) * embedding * local;
}

inline glm::mat4 cameraRelativeTransform(glm::ivec3 origin, const glm::mat4 &local,
                                         glm::dvec3 camera) {
    glm::mat4 result = local;
    const glm::dvec3 relative = glm::dvec3(origin) + glm::dvec3(local[3]) - camera;
    result[3] = glm::vec4(glm::vec3(relative), local[3].w);
    return result;
}

inline glm::mat4 instanceLightTransform(int scene, glm::ivec3 origin,
                                       const glm::mat4 &lighting, const glm::mat4 &local) {
    const glm::mat4 relative = lighting * local;
    return scene == 0 ? glm::translate(glm::mat4(1), glm::vec3(origin)) * relative : relative;
}

inline bool validInstanceBytes(int adapter, int bytes) {
    constexpr std::array<int, 8> sizes{76, 112, 52, 36, 52, 72, 108, 88};
    return adapter >= 0 && static_cast<size_t>(adapter) < sizes.size() && sizes[adapter] == bytes;
}

// Ray tracing later applies inverse-transpose(objectPose) to the material normal.
// Pre-correct it so Flywheel's explicit normal transform survives that operation.
inline glm::mat3 normalCorrection(const glm::mat4 &objectPose, const glm::mat3 &normalPose) {
    return glm::transpose(glm::mat3(objectPose)) * normalPose;
}

inline int32_t signExtend(uint64_t value, unsigned bits) {
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
}

inline glm::ivec3 sectionCoordinate(int64_t section) {
    const uint64_t packed = static_cast<uint64_t>(section);
    return {signExtend(packed >> 42, 22), signExtend(packed & 0xFFFFFu, 20),
            signExtend((packed >> 20) & 0x3FFFFFu, 22)};
}

inline bool isFlywheelAlphaMode(uint32_t mode) {
    return mode == 11u || (mode >= 20u && mode <= 23u);
}

enum class FlywheelBlend : uint32_t {
    Additive = 1,
    Lightning = 2,
    Glint = 3,
    Crumbling = 4,
    Translucent = 5,
};

inline glm::vec4 blendFlywheel(glm::vec4 source, glm::vec4 destination, FlywheelBlend blend) {
    switch (blend) {
    case FlywheelBlend::Additive:
        return source + destination;
    case FlywheelBlend::Lightning:
        return source * source.a + destination;
    case FlywheelBlend::Glint:
        return {glm::vec3(source) * glm::vec3(source) + glm::vec3(destination), destination.a};
    case FlywheelBlend::Crumbling:
        return {2.0f * glm::vec3(source) * glm::vec3(destination), source.a};
    case FlywheelBlend::Translucent:
        return {glm::vec3(source) * source.a + glm::vec3(destination) * (1.0f - source.a),
                source.a + destination.a * (1.0f - source.a)};
    }
    throw std::out_of_range("Unknown Flywheel blend mode");
}

inline float chunkDiffuse(glm::vec3 normal, bool constantAmbientLight) {
    const glm::vec3 squared = normal * normal;
    if (constantAmbientLight)
        return std::min(squared.x * 0.6f + squared.y * 0.9f + squared.z * 0.8f, 1.0f);
    return std::min(squared.x * 0.6f + squared.y * 0.25f * (3.0f + normal.y)
                    + squared.z * 0.8f, 1.0f);
}

inline glm::vec3 crumblingPosition(const glm::mat4 &instanceTransform, glm::vec3 localPosition) {
    return glm::vec3(instanceTransform * glm::vec4(localPosition, 1.0f));
}
}
