#pragma once
#include <cstdint>
#include "common/shared.hpp"
#include "core/render/material_faces.hpp"
#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcvr {
// One entry per published Chunk1, protected by the existing Chunks mutex. All
// referenced arrays are immutable after publication and kept alive by owner.
struct ChunkSceneKey {
    int64_t version = -1;
    uint32_t count = 0;
    std::array<const void *, 11> resources{};
    bool operator==(const ChunkSceneKey &) const = default;
};

struct ChunkSceneMetadata {
    std::shared_ptr<void> owner;
    faces::ModelRules faces;
    std::vector<std::string> groups;
    std::span<const uint64_t> indices, positions, materials;
    std::vector<vk::VertexFormat::InstanceAppearance> appearances;

    ChunkSceneMetadata(std::shared_ptr<void> retainedOwner, uint32_t count,
        std::span<const uint64_t> indexAddresses, std::span<const uint64_t> positionAddresses,
        std::span<const uint64_t> materialAddresses, std::span<const std::string> names,
        std::span<const uint32_t> materialFlags)
        : owner(std::move(retainedOwner)), faces(materialFlags) {
        if (!owner || indexAddresses.size() < count || positionAddresses.size() < count ||
            materialAddresses.size() < count || materialFlags.size() < count)
            throw std::invalid_argument("Incomplete published chunk scene metadata");
        indices = indexAddresses.first(count); positions = positionAddresses.first(count);
        materials = materialAddresses.first(count);
        groups.reserve(size_t(count) + 1); groups.emplace_back("shadow");
        appearances.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            groups.push_back(i < names.size() ? names[i] : "default");
            vk::VertexFormat::InstanceAppearance appearance{};
            appearance.colorMultiply = glm::vec4(1.0f);
            appearance.colorReplace = glm::vec4(1.0f);
            appearance.fluidProgress = 1.0f;
            appearance.materialFlags = faces.shaderFlags(materialFlags[i]);
            appearances.push_back(appearance);
        }
    }

    void append(bool customTransform, std::vector<std::string> &hitGroups,
        std::vector<uint64_t> &indexAddresses, std::vector<uint64_t> &positionAddresses,
        std::vector<uint64_t> &materialAddresses, std::vector<uint64_t> &lastIndices,
        std::vector<uint64_t> &lastPositions,
        std::vector<vk::VertexFormat::InstanceAppearance> &instanceAppearances) const {
        hitGroups.insert(hitGroups.end(), groups.begin(), groups.end());
        indexAddresses.insert(indexAddresses.end(), indices.begin(), indices.end());
        positionAddresses.insert(positionAddresses.end(), positions.begin(), positions.end());
        materialAddresses.insert(materialAddresses.end(), materials.begin(), materials.end());
        instanceAppearances.insert(instanceAppearances.end(), appearances.begin(), appearances.end());
        if (customTransform) {
            lastIndices.insert(lastIndices.end(), indices.begin(), indices.end());
            lastPositions.insert(lastPositions.end(), positions.begin(), positions.end());
        } else {
            lastIndices.insert(lastIndices.end(), indices.size(), 0);
            lastPositions.insert(lastPositions.end(), positions.size(), 0);
        }
    }
};

class ChunkSceneCache {
    ChunkSceneKey key_{};
    std::shared_ptr<ChunkSceneMetadata> value_;
    uint64_t builds_ = 0;
public:
    template<class Factory>
    const std::shared_ptr<ChunkSceneMetadata> &get(const ChunkSceneKey &key, Factory &&factory) {
        if (!value_ || !(key == key_)) {
            auto next = factory(); // Failure leaves the previous frame's owner intact.
            if (!next) throw std::invalid_argument("Missing chunk scene snapshot");
            value_ = std::move(next); key_ = key; ++builds_;
        }
        return value_;
    }
    void reset() noexcept { value_.reset(); }
    uint64_t builds() const noexcept { return builds_; }
};

template<class Map, class Key>
void recordChunkTransform(Map &history, const Key &key, const glm::dmat4 &transform,
                          bool needsHistory, bool reference) {
    // Keep history for custom-capable external slots even before their first transform.
    if (needsHistory || reference) history[key] = transform;
}
template<class Map, class Key>
glm::dmat4 previousChunkTransform(const Map &history, const Key &key,
                                 const glm::dmat4 &current, bool custom) {
    if (custom) {
        auto previous = history.find(key);
        if (previous != history.end()) return previous->second;
    }
    return current;
}
} // namespace mcvr
