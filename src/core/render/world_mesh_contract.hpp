#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace WorldMeshContract {

inline std::vector<uint32_t> triangulate(int mode,
                                         const std::vector<uint32_t> &drawIndices) {
    std::vector<uint32_t> triangles;
    switch (mode) {
        case 4: // TRIANGLES
        case 7: // QUADS, whose supplied indices are already a triangle list
            if (drawIndices.size() % 3 != 0) {
                throw std::runtime_error("World triangle index count is not divisible by three");
            }
            return drawIndices;
        case 5: // TRIANGLE_STRIP
            triangles.reserve(drawIndices.size() < 3 ? 0 : (drawIndices.size() - 2) * 3);
            for (size_t i = 2; i < drawIndices.size(); ++i) {
                if ((i & 1u) == 0u) {
                    triangles.insert(triangles.end(),
                        {drawIndices[i - 2], drawIndices[i - 1], drawIndices[i]});
                } else {
                    triangles.insert(triangles.end(),
                        {drawIndices[i - 1], drawIndices[i - 2], drawIndices[i]});
                }
            }
            return triangles;
        case 6: // TRIANGLE_FAN
            triangles.reserve(drawIndices.size() < 3 ? 0 : (drawIndices.size() - 2) * 3);
            for (size_t i = 2; i < drawIndices.size(); ++i) {
                triangles.insert(triangles.end(),
                    {drawIndices[0], drawIndices[i - 1], drawIndices[i]});
            }
            return triangles;
        default:
            throw std::runtime_error("Indexed world sink accepts surface triangle topologies only");
    }
}

inline bool generationMatches(uint64_t activeWorld, uint64_t activeFrame,
                              uint64_t activeResources, uint64_t world, uint64_t frame,
                              uint64_t resources) {
    return world != 0 && frame != 0 && resources != 0 && activeWorld == world &&
           activeFrame == frame && activeResources == resources;
}

inline std::array<double, 3> worldTranslation(double originX, double originY, double originZ,
                                               double cameraX, double cameraY, double cameraZ) {
    return {originX - cameraX, originY - cameraY, originZ - cameraZ};
}

inline uint32_t alphaMode(bool pbrSource, uint32_t encodedAlphaMode,
                          uint32_t renderTypeAlphaMode) {
    return pbrSource ? encodedAlphaMode : renderTypeAlphaMode;
}

inline float emission(float encodedEmission, float renderTypeEmission) {
    return std::max(encodedEmission, renderTypeEmission);
}

} // namespace WorldMeshContract
