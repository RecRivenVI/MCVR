#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace mcvr::chunkGeometry {

// Upload layout of one chunk-build batch. The batch packs all position, material and index
// streams into one transient staging buffer, but every chunk gets its own device-local streams
// (and its own BLAS storage), so unloading or replacing a chunk frees exactly its memory instead
// of pinning every buffer shared with the other chunks of the batch it was built in.

struct Geometry {
    uint64_t vertices = 0, indices = 0;
};

struct Strides {
    uint64_t position = 0, material = 0, index = 0;
};

// Byte range in the shared staging buffer; the destination is offset 0 of the chunk's own buffer.
struct Copy {
    uint64_t stagingOffset = 0, bytes = 0;
};

struct Chunk {
    uint64_t vertexCount = 0, indexCount = 0;
    Copy positions, materials, indices;
    // Per geometry, in elements, relative to the start of this chunk's own buffers.
    std::vector<uint64_t> vertexOffsets, indexOffsets;
};

struct Batch {
    std::vector<Chunk> chunks;
    uint64_t geometryCount = 0, vertexCount = 0, indexCount = 0;
    // Staging holds [all positions][all materials][all indices] in batch order.
    uint64_t stagingBytes = 0;
};

// Vulkan buffers cannot be empty; a chunk whose geometries carry no data still needs a handle.
inline uint64_t allocationBytes(uint64_t bytes) { return std::max<uint64_t>(bytes, 16); }

// counts[c][g] describes geometry g of chunk c in the order the batch packs them.
inline Batch plan(std::span<const std::vector<Geometry>> counts, Strides strides) {
    if (strides.position == 0 || strides.material == 0 || strides.index == 0)
        throw std::invalid_argument("Chunk geometry strides must be nonzero");
    Batch batch;
    batch.chunks.resize(counts.size());
    for (size_t c = 0; c < counts.size(); ++c) {
        auto &chunk = batch.chunks[c];
        chunk.vertexOffsets.reserve(counts[c].size());
        chunk.indexOffsets.reserve(counts[c].size());
        chunk.positions.stagingOffset = batch.vertexCount * strides.position;
        chunk.indices.stagingOffset = batch.indexCount * strides.index;
        for (const auto &geometry : counts[c]) {
            chunk.vertexOffsets.push_back(chunk.vertexCount);
            chunk.indexOffsets.push_back(chunk.indexCount);
            chunk.vertexCount += geometry.vertices;
            chunk.indexCount += geometry.indices;
        }
        chunk.materials.stagingOffset = batch.vertexCount * strides.material;
        chunk.positions.bytes = chunk.vertexCount * strides.position;
        chunk.materials.bytes = chunk.vertexCount * strides.material;
        chunk.indices.bytes = chunk.indexCount * strides.index;
        batch.geometryCount += counts[c].size();
        batch.vertexCount += chunk.vertexCount;
        batch.indexCount += chunk.indexCount;
    }
    const uint64_t materialBase = batch.vertexCount * strides.position;
    const uint64_t indexBase = materialBase + batch.vertexCount * strides.material;
    for (auto &chunk : batch.chunks) {
        chunk.materials.stagingOffset += materialBase;
        chunk.indices.stagingOffset += indexBase;
    }
    batch.stagingBytes = indexBase + batch.indexCount * strides.index;
    return batch;
}

} // namespace mcvr::chunkGeometry
