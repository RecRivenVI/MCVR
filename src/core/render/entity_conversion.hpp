#pragma once
#include "common/shared.hpp"
#include "common/entity_convert.hpp"
#include "core/vulkan/vertex.hpp"
#include <bit>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace mcvr {
static_assert(sizeof(vk::VertexFormat::PBRVertex) == 128);
static_assert(offsetof(vk::VertexFormat::PBRVertex, alphaMode) == 124);
static_assert(offsetof(vk::VertexFormat::PBRVertex, coordinate) == 104);
// Explicit trial boundary. Special CPU consumers retain their original input path.
struct EntityConversionInput {
    int format, drawMode, count;
    bool post, external, explicitIndices, eyeLayer, cached, prebuilt;
};
inline bool rawEntityConversionEligible(const EntityConversionInput &v) noexcept {
    return v.format == 12 && v.count > 0 &&
        ((v.drawMode == 7 && v.count % 4 == 0) || (v.drawMode == 4 && v.count % 3 == 0)) &&
        !v.post && !v.external && !v.explicitIndices && !v.eyeLayer && !v.cached && !v.prebuilt;
}
struct EntityRawGeometry {
    std::unique_ptr<uint32_t[]> words;
    uint32_t wordCount = 0;
    EntityConvertJob parameters{};
    bool present() const noexcept { return words != nullptr; }
};
inline uint32_t entityWordCount(size_t bytes) {
    if (bytes % 4 || bytes / 4 > UINT32_MAX) throw std::length_error("Entity conversion input exceeds word address space");
    return static_cast<uint32_t>(bytes / 4);
}
inline void appendEntityTiles(std::vector<EntityConvertJob> &jobs, EntityConvertJob job) {
    const uint64_t work = std::max(job.vertexCount, job.indexCount);
    if (jobs.size() + (work + 63) / 64 > UINT32_MAX)
        throw std::length_error("Entity conversion tile count overflow");
    for (uint64_t first = 0; first < work; first += 64) {
        job.first = static_cast<uint32_t>(first);
        jobs.push_back(job);
    }
}
}
