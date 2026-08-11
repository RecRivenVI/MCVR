#pragma once
#include "common/shared.hpp"
#include "common/entity_convert.hpp"
#include "common/pbr_source.hpp"
#include "core/vulkan/vertex.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace vk { class DeviceLocalBuffer; }

namespace mcvr {
static_assert(sizeof(vk::VertexFormat::PBRVertex) == 128);
static_assert(offsetof(vk::VertexFormat::PBRVertex, alphaMode) == 124);
static_assert(offsetof(vk::VertexFormat::PBRVertex, coordinate) == 104);
inline constexpr size_t maxEntityDirectInputBytes = 64u * 1024u * 1024u;

inline size_t checkedEntitySourceBytes(uint64_t vertexCount, size_t stride,
                                       uint64_t declaredBytes) {
    if (!stride) throw std::invalid_argument("Entity vertex source stride is zero");
    if (vertexCount > std::numeric_limits<size_t>::max() / stride)
        throw std::length_error("Entity vertex source byte count overflow");
    const size_t required = static_cast<size_t>(vertexCount) * stride;
    if (required != declaredBytes)
        throw std::length_error("Entity vertex source length does not match its declared format");
    return required;
}

inline size_t entitySourceByteCount(uint64_t vertexCount, size_t stride) {
    if (!stride) throw std::invalid_argument("Entity vertex source stride is zero");
    if (vertexCount > std::numeric_limits<size_t>::max() / stride)
        throw std::length_error("Entity vertex source byte count overflow");
    return static_cast<size_t>(vertexCount) * stride;
}

inline void validateEntitySourceAddress(const void *address, size_t byteCount) {
    if (byteCount && address == nullptr)
        throw std::invalid_argument("Entity source address is null for a nonempty vertex stream");
}

class EntityDirectInputLayout {
  public:
    explicit EntityDirectInputLayout(size_t maximumBytes = maxEntityDirectInputBytes)
        : maximumBytes_(maximumBytes) {}

    size_t append(size_t bytes) {
        if (bytes > std::numeric_limits<size_t>::max() - totalBytes_)
            throw std::length_error("Entity direct input byte count overflow");
        const size_t offset = totalBytes_;
        totalBytes_ += bytes;
        withinLimit_ = withinLimit_ && totalBytes_ <= maximumBytes_;
        return offset;
    }

    size_t totalBytes() const noexcept { return totalBytes_; }
    bool withinLimit() const noexcept { return withinLimit_; }

  private:
    size_t maximumBytes_;
    size_t totalBytes_ = 0;
    bool withinLimit_ = true;
};

class EntityDirectInputBudget {
  public:
    static bool tryReserve(size_t bytes) noexcept {
        if (!bytes || bytes > maxEntityDirectInputBytes) return false;
        size_t used = usedBytes_.load(std::memory_order_relaxed);
        for (;;) {
            if (bytes > maxEntityDirectInputBytes - used) return false;
            if (usedBytes_.compare_exchange_weak(used, used + bytes,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
        }
    }

    static void release(size_t bytes) noexcept {
        usedBytes_.fetch_sub(bytes, std::memory_order_acq_rel);
    }

    static size_t reservedBytes() noexcept {
        return usedBytes_.load(std::memory_order_acquire);
    }

  private:
    inline static std::atomic_size_t usedBytes_{0};
};

struct EntityDirectInputBatch {
    std::shared_ptr<vk::DeviceLocalBuffer> buffer;
    size_t byteCount = 0;
    bool ownsBudgetReservation = false;

    EntityDirectInputBatch() = default;
    EntityDirectInputBatch(const EntityDirectInputBatch &) = delete;
    EntityDirectInputBatch &operator=(const EntityDirectInputBatch &) = delete;
    EntityDirectInputBatch(EntityDirectInputBatch &&) = delete;
    EntityDirectInputBatch &operator=(EntityDirectInputBatch &&) = delete;

    ~EntityDirectInputBatch() noexcept {
        buffer.reset();
        if (ownsBudgetReservation) EntityDirectInputBudget::release(byteCount);
    }
};

struct EntitySourceJobGroup {
    std::uintptr_t sourceKey = 0;
    std::vector<EntityConvertJob> tiles;
};

inline size_t entitySourceJobGroupIndex(std::vector<EntitySourceJobGroup> &groups,
                                        std::uintptr_t sourceKey) {
    const auto found = std::find_if(groups.begin(), groups.end(),
        [sourceKey](const EntitySourceJobGroup &group) { return group.sourceKey == sourceKey; });
    if (found != groups.end()) return static_cast<size_t>(found - groups.begin());
    groups.push_back(EntitySourceJobGroup{.sourceKey = sourceKey});
    return groups.size() - 1;
}

// Explicit trial boundary. Special CPU consumers retain their original input path.
struct EntityConversionInput {
    int format, drawMode, count;
    bool post, external, explicitIndices, eyeLayer, cached, prebuilt;
};
inline bool rawEntityConversionEligible(const EntityConversionInput &v) noexcept {
    return (v.format == legacyPbrSourceFormat || v.format == compactPbrSourceFormat) && v.count > 0 &&
        ((v.drawMode == 7 && v.count % 4 == 0) || (v.drawMode == 4 && v.count % 3 == 0)) &&
        !v.post && !v.external && !v.explicitIndices && !v.eyeLayer && !v.cached && !v.prebuilt;
}
struct EntityRawGeometry {
    std::unique_ptr<uint32_t[]> words;
    uint32_t wordCount = 0;
    EntityConvertJob parameters{};
    // Direct submission spans exist only inside Entities::queueBuild before its staging fill.
    const std::byte *pendingSource = nullptr;
    size_t pendingSourceBytes = 0;
    size_t pendingSourceOffsetBytes = 0;
    std::shared_ptr<EntityDirectInputBatch> directInput;
    uint32_t sourceWordOffset = 0;
    bool deferred = false;
    bool present() const noexcept { return deferred; }
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
