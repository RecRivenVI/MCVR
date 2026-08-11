#include "core/render/overlay_buffer_reuse.hpp"

#include <memory>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("overlay transient usage/lifetime regression");
}
struct Buffer {
    size_t capacity;
    VkBufferUsageFlags usage;
    size_t size() const { return capacity; }
    VkBufferUsageFlags usageFlags() const { return usage; }
};
} // namespace

int main() {
    constexpr auto vertex = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    constexpr auto index = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    std::shared_ptr<Buffer> slot;
    std::vector<std::shared_ptr<Buffer>> inFlight;
    int creates = 0;
    auto create = [&](size_t capacity, VkBufferUsageFlags usage) {
        ++creates;
        return std::make_shared<Buffer>(Buffer{capacity, usage});
    };
    auto retain = [&](const std::shared_ptr<Buffer> &old) { inFlight.push_back(old); };

    require(mcvr::render::ensureOverlayBuffer(slot, 1024, vertex, 16 * 1024, create, retain));
    require(slot->size() == 16 * 1024 && slot->usageFlags() == vertex && creates == 1);
    require(!mcvr::render::ensureOverlayBuffer(slot, 64, vertex, 16 * 1024, create, retain));
    require(creates == 1 && inFlight.empty());

    // A resetFrame reuses this numeric ID as an index slot with the same
    // 16 KiB capacity. The old vertex-only VkBuffer is not legal for indices.
    std::weak_ptr<Buffer> oldVertex = slot;
    require(mcvr::render::ensureOverlayBuffer(slot, 96, index, 16 * 1024, create, retain));
    require(creates == 2 && slot->size() == 16 * 1024
        && (slot->usageFlags() & (vertex | index)) == (vertex | index));
    require(inFlight.size() == 1 && !oldVertex.expired());
    require(!mcvr::render::ensureOverlayBuffer(slot, 1024, vertex, 16 * 1024, create, retain));
    require(!mcvr::render::ensureOverlayBuffer(slot, 96, index, 16 * 1024, create, retain));
    require(creates == 2); // role alternation no longer thrashes allocations

    std::weak_ptr<Buffer> oldDualUse = slot;
    require(mcvr::render::ensureOverlayBuffer(slot, 20 * 1024, vertex, 16 * 1024, create, retain));
    require(slot->size() == 32 * 1024 && creates == 3 && inFlight.size() == 2);
    require(!oldDualUse.expired()); // resize retains the previous in-flight buffer too
    inFlight.clear();
    require(oldVertex.expired() && oldDualUse.expired());
}
