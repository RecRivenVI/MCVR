#include "core/render/external_chunk_handle.hpp"

#include <stdexcept>

int main() {
    mcvr::ExternalChunkHandleTable handles;
    handles.reset(8);
    if (handles.resolve(7) != 7 || handles.resolve(8).has_value())
        throw std::runtime_error("Primary handle boundary is invalid");

    auto first = handles.allocate(8);
    if (first.slot != 8 || first.reused || handles.resolve(first.handle) != 8)
        throw std::runtime_error("First external slot allocation failed");
    const uint32_t firstGeneration = handles.generation(first.slot);
    if (!handles.release(first.handle) || handles.resolve(first.handle).has_value())
        throw std::runtime_error("Released handle remained valid");

    auto second = handles.allocate(9);
    if (second.slot != first.slot || !second.reused || second.handle == first.handle
        || handles.generation(second.slot) == firstGeneration)
        throw std::runtime_error("External slot was not generation-safely reused");
    if (handles.release(first.handle).has_value())
        throw std::runtime_error("Stale release invalidated a reused slot");
    if (!handles.isCurrent(second.slot, handles.generation(second.slot)))
        throw std::runtime_error("Current generation was rejected");
    auto current = second;
    for (int i = 0; i < 100000; ++i) {
        const auto staleHandle = current.handle;
        const auto staleGeneration = handles.generation(current.slot);
        if (!handles.release(current.handle))
            throw std::runtime_error("Churn release failed");
        current = handles.allocate(9);
        if (current.slot != 8 || handles.resolve(staleHandle).has_value()
            || handles.isCurrent(current.slot, staleGeneration))
            throw std::runtime_error("Generation-safe churn regressed");
    }
    return 0;
}
