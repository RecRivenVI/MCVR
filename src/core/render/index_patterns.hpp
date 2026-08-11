#pragma once

#include <stdexcept>
#include <vector>

namespace mcvr::render {

enum class FourVertexIndexPattern {
    MINECRAFT_LINES,
    QUADS,
};

template <typename Index>
std::vector<Index> buildFourVertexIndices(int vertexCount, int expectedIndexCount, FourVertexIndexPattern pattern) {
    if (vertexCount < 0 || vertexCount % 4 != 0) { throw std::runtime_error("four-vertex primitive count is invalid"); }

    int indexCount = vertexCount / 4 * 6;
    if (indexCount != expectedIndexCount) { throw std::runtime_error("index count not match!"); }

    std::vector<Index> indices;
    indices.reserve(indexCount);
    for (int i = 0; i < vertexCount; i += 4) {
        indices.push_back(static_cast<Index>(i + 0));
        indices.push_back(static_cast<Index>(i + 1));
        indices.push_back(static_cast<Index>(i + 2));
        if (pattern == FourVertexIndexPattern::MINECRAFT_LINES) {
            indices.push_back(static_cast<Index>(i + 3));
            indices.push_back(static_cast<Index>(i + 2));
            indices.push_back(static_cast<Index>(i + 1));
        } else {
            indices.push_back(static_cast<Index>(i + 2));
            indices.push_back(static_cast<Index>(i + 3));
            indices.push_back(static_cast<Index>(i + 0));
        }
    }
    return indices;
}

} // namespace mcvr::render
