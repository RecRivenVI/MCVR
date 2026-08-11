#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace mcvr::diagnostics {

// The trace is intentionally opt-in. No file is opened and no state is changed
// unless MCVR_DRAW_STATE_TRACE_PATH names an output file in the process environment.
bool drawStateTraceEnabled() noexcept;

void recordDescriptorLifecycle(std::string_view event, uint64_t table,
                               uint64_t layout, uint64_t pool) noexcept;

void recordFrame(std::string_view event,
                 uint32_t frameIndex,
                 bool frameSubmitted,
                 uint64_t fence,
                 uint64_t overlayCommandBuffer) noexcept;

void recordUniform(std::string_view source,
                   uint32_t frameIndex,
                   bool frameSubmitted,
                   uint32_t shaderId,
                   uint32_t vertexId,
                   uint32_t indexId,
                   int32_t patchIndexId,
                   uint32_t uniformOffset,
                   const uint8_t *data,
                   size_t size) noexcept;

void recordDescriptorImage(uint32_t frameIndex,
                           uint64_t descriptorTableObject,
                           uint64_t pipelineLayout,
                           uint64_t descriptorSet,
                           int32_t set,
                           int32_t binding,
                           int32_t index,
                           uint64_t samplerObject,
                           uint64_t sampler,
                           uint64_t imageObject,
                           uint64_t image,
                           uint64_t imageView,
                           uint32_t imageLayout) noexcept;

void recordDescriptorBuffer(uint32_t frameIndex,
                            uint64_t descriptorTableObject,
                            uint64_t pipelineLayout,
                            uint64_t descriptorSet,
                            int32_t set,
                            int32_t binding,
                            uint64_t bufferObject,
                            uint64_t buffer,
                            uint64_t descriptorOffset,
                            uint64_t descriptorRange,
                            uint64_t bufferSize) noexcept;

void recordTexture(std::string_view event,
                   uint32_t textureId,
                   uint32_t fallbackId,
                   uint32_t frameIndex,
                   bool frameSubmitted,
                   uint64_t imageObject,
                   uint64_t image,
                   uint64_t samplerObject,
                   uint64_t sampler,
                   uint32_t width,
                   uint32_t height,
                   uint32_t mipLevels,
                   uint32_t format,
                   uint64_t queuedUploadBytes) noexcept;

void recordDraw(std::string_view source,
                uint32_t frameIndex,
                bool frameSubmitted,
                uint32_t overlayMode,
                uint32_t shaderId,
                std::string_view shaderKey,
                uint64_t commandBuffer,
                uint64_t pipeline,
                uint64_t pipelineLayout,
                uint64_t descriptorTableObject,
                std::span<const uint64_t> descriptorSets,
                uint64_t vertexObject,
                uint64_t vertexBuffer,
                uint64_t vertexBufferSize,
                uint64_t indexObject,
                uint64_t indexBuffer,
                uint64_t indexBufferSize,
                uint64_t patchIndexObject,
                uint64_t patchIndexBuffer,
                uint64_t patchIndexBufferSize,
                uint32_t uniformOffset,
                uint32_t indexCount,
                uint32_t patchIndexCount,
                uint32_t instanceCount,
                uint32_t indexType) noexcept;

template <typename T>
uint64_t handleValue(T value) noexcept {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<uint64_t>(value);
    } else {
        return static_cast<uint64_t>(value);
    }
}

} // namespace mcvr::diagnostics
