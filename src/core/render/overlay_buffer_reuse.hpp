#pragma once

#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace mcvr::render {

// Transient numeric overlay IDs recur each frame. Capacity alone is insufficient:
// a slot previously used for vertices may now be bound as an index buffer.
template <typename Buffer, typename Create, typename Retain>
bool ensureOverlayBuffer(std::shared_ptr<Buffer> &slot, size_t requestedSize,
                         VkBufferUsageFlags requiredUsage, size_t baseCapacity,
                         Create &&create, Retain &&retain) {
    size_t capacity = slot ? slot->size() : baseCapacity;
    if (capacity == 0) capacity = 1;
    while (capacity < requestedSize) {
        if (capacity > std::numeric_limits<size_t>::max() / 2)
            throw std::overflow_error("Overlay buffer capacity overflow");
        capacity *= 2;
    }
    const VkBufferUsageFlags existingUsage = slot ? slot->usageFlags() : 0;
    if (slot && capacity == slot->size()
        && (existingUsage & requiredUsage) == requiredUsage) return false;
    // Retain legal prior usage bits so a slot that alternates roles does not
    // reallocate every frame. The constructor still adds transfer usage.
    auto replacement = create(capacity, existingUsage | requiredUsage);
    if (slot) retain(slot); // old in-flight commands may still reference it
    slot = std::move(replacement);
    return true;
}

} // namespace mcvr::render
