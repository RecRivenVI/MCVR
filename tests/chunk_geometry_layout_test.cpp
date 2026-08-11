#include "core/render/chunk_geometry_layout.hpp"

#include <stdexcept>
#include <vector>

using namespace mcvr::chunkGeometry;

namespace {
void require(bool value, const char *what) {
    if (!value) throw std::runtime_error(what);
}
} // namespace

int main() {
    // Distinct strides so a position/material/index mix-up changes every offset.
    const Strides strides{16, 32, 4};
    const std::vector<std::vector<Geometry>> counts{
        {{3, 6}, {4, 6}}, // chunk 0: two geometries
        {},               // chunk 1: nothing to draw
        {{5, 9}},         // chunk 2
    };
    const auto batch = plan(counts, strides);
    require(batch.chunks.size() == 3, "one plan per chunk, including empty ones");
    require(batch.geometryCount == 3 && batch.vertexCount == 12 && batch.indexCount == 21, "batch totals");
    require(batch.stagingBytes == 12 * 16 + 12 * 32 + 21 * 4, "staging holds every stream once");

    const auto &first = batch.chunks[0];
    require(first.vertexCount == 7 && first.indexCount == 12, "chunk totals");
    require(first.vertexOffsets == std::vector<uint64_t>{0, 3}, "geometry vertex offsets are chunk-relative");
    require(first.indexOffsets == std::vector<uint64_t>{0, 6}, "geometry index offsets are chunk-relative");
    require(first.positions.stagingOffset == 0 && first.positions.bytes == 7 * 16, "first positions");
    require(first.materials.stagingOffset == 12 * 16 && first.materials.bytes == 7 * 32, "first materials");
    require(first.indices.stagingOffset == 12 * 16 + 12 * 32 && first.indices.bytes == 12 * 4, "first indices");

    const auto &empty = batch.chunks[1];
    require(empty.vertexCount == 0 && empty.indexCount == 0 && empty.vertexOffsets.empty(), "empty chunk");
    require(empty.positions.bytes == 0 && empty.materials.bytes == 0 && empty.indices.bytes == 0,
            "an empty chunk copies nothing");

    // A later chunk starts where the previous one ended in every stream, and its geometry
    // offsets restart at zero because it owns separate buffers.
    const auto &last = batch.chunks[2];
    require(last.vertexOffsets == std::vector<uint64_t>{0} && last.indexOffsets == std::vector<uint64_t>{0},
            "offsets restart per chunk");
    require(last.positions.stagingOffset == 7 * 16 && last.positions.bytes == 5 * 16, "last positions");
    require(last.materials.stagingOffset == 12 * 16 + 7 * 32 && last.materials.bytes == 5 * 32, "last materials");
    require(last.indices.stagingOffset == 12 * 16 + 12 * 32 + 12 * 4 && last.indices.bytes == 9 * 4,
            "last indices");

    // The copies tile the staging buffer exactly: no gaps, no overlap, nothing past the end.
    uint64_t covered = 0;
    for (const auto &chunk : batch.chunks) covered += chunk.positions.bytes + chunk.materials.bytes + chunk.indices.bytes;
    require(covered == batch.stagingBytes, "copies cover staging exactly once");
    require(last.indices.stagingOffset + last.indices.bytes == batch.stagingBytes, "last copy ends at staging end");

    require(allocationBytes(0) > 0 && allocationBytes(4096) == 4096, "allocation size is never zero");
    require(plan({}, strides).stagingBytes == 0, "empty batch");

    bool rejected = false;
    try { (void)plan(counts, {0, 32, 4}); } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "zero strides are rejected");
    return 0;
}
