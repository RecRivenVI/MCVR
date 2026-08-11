#include "com_radiance_client_proxy_vulkan_VertexArrayProxy.h"

#include "core/middleware/jni_exception.hpp"
#include "core/render/buffers.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/vulkan/device.hpp"
#include "core/vulkan/physical_device.hpp"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

namespace {
struct ArrayBinding {
    uint32_t bufferId;
    uint32_t offset;
    VkVertexInputBindingDescription description;
};

struct NativeVertexArray {
    std::map<uint32_t, ArrayBinding> bindings;
    std::map<uint32_t, VkVertexInputAttributeDescription> attributes;
    uint32_t indexBufferId = UINT32_MAX;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    uint32_t indexCount = 0;
};

std::mutex arraysMutex;
std::map<uint32_t, NativeVertexArray> arrays;
std::atomic_uint32_t nextArrayId{1};

VkFormat attributeFormat(int components, int type, bool normalized, bool integer) {
    if (components < 1 || components > 4) throw std::invalid_argument("Invalid vertex component count");
    auto pick = [&](VkFormat r1, VkFormat r2, VkFormat r3, VkFormat r4) {
        switch (components) {
            case 1: return r1;
            case 2: return r2;
            case 3: return r3;
            default: return r4;
        }
    };
    switch (type) {
        case 0: // BYTE
            if (integer) return pick(VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT);
            if (normalized) return pick(VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM);
            return pick(VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED);
        case 1: // UNSIGNED_BYTE
            if (integer) return pick(VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT);
            if (normalized) return pick(VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM);
            return pick(VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED);
        case 2: // SHORT
            if (integer) return pick(VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT);
            if (normalized) return pick(VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM);
            return pick(VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED);
        case 3: // UNSIGNED_SHORT
            if (integer) return pick(VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT);
            if (normalized) return pick(VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM);
            return pick(VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED);
        case 4: // INT
            if (normalized) throw std::invalid_argument("Integer attributes cannot be normalized");
            if (integer) return pick(VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT);
            throw std::invalid_argument("32-bit integer attributes must be declared as integer");
        case 5: // UNSIGNED_INT
            if (normalized) throw std::invalid_argument("Integer attributes cannot be normalized");
            if (integer) return pick(VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT);
            throw std::invalid_argument("32-bit integer attributes must be declared as integer");
        case 6: // FLOAT
            if (integer) throw std::invalid_argument("Floating point attributes cannot be integer");
            return pick(VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT);
        default:
            throw std::invalid_argument("Unsupported custom vertex component type");
    }
}

NativeVertexArray copyArray(uint32_t id) {
    std::lock_guard lock(arraysMutex);
    auto found = arrays.find(id);
    if (found == arrays.end()) throw std::runtime_error("Unknown native VertexArray");
    return found->second;
}

void validateLayout(const NativeVertexArray &array, const OverlayDynamicDrawShaderInfo &shader) {
    if (!shader.customVertexLayout)
        throw std::runtime_error("Custom VertexArray shader has no explicit layout");
    if (array.bindings.size() != shader.vertexLayout.bindingDescriptions.size() ||
        array.attributes.size() != shader.vertexLayout.attributeDescriptions.size())
        throw std::runtime_error("Custom VertexArray layout is incomplete");
    for (const auto &expected : shader.vertexLayout.bindingDescriptions) {
        auto found = array.bindings.find(expected.binding);
        if (found == array.bindings.end() ||
            found->second.description.stride != expected.stride ||
            found->second.description.inputRate != expected.inputRate)
            throw std::runtime_error("Custom VertexArray binding does not match shader layout");
    }
    for (const auto &expected : shader.vertexLayout.attributeDescriptions) {
        auto found = array.attributes.find(expected.location);
        if (found == array.attributes.end() || found->second.binding != expected.binding ||
            found->second.format != expected.format || found->second.offset != expected.offset)
            throw std::runtime_error("Custom VertexArray attribute does not match shader layout");
    }
}

uint32_t readIndex(vk::DeviceLocalBuffer &buffer, VkIndexType type, uint32_t index) {
    const auto *bytes = static_cast<const uint8_t *>(buffer.mappedPtr());
    if (!bytes) throw std::runtime_error("Custom VertexArray index staging data is unavailable");
    if (type == VK_INDEX_TYPE_UINT16) {
        uint16_t value;
        std::memcpy(&value, bytes + index * sizeof(value), sizeof(value));
        return value;
    }
    uint32_t value;
    std::memcpy(&value, bytes + index * sizeof(value), sizeof(value));
    return value;
}

void validateCommand(const NativeVertexArray &array,
                     const std::map<uint32_t, std::shared_ptr<vk::DeviceLocalBuffer>> &resolved,
                     vk::DeviceLocalBuffer &indexBuffer,
                     const VkDrawIndexedIndirectCommand &command) {
    if (command.firstIndex > array.indexCount || command.indexCount > array.indexCount - command.firstIndex)
        throw std::out_of_range("Sable indirect firstIndex/count exceeds converted index buffer");
    for (const auto &[binding, value] : array.bindings) {
        auto buffer = resolved.at(binding);
        if (value.offset > buffer->size())
            throw std::out_of_range("Sable vertex binding offset exceeds buffer");
        const uint64_t capacity = (buffer->size() - value.offset) / value.description.stride;
        if (value.description.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
            const uint64_t end = static_cast<uint64_t>(command.firstInstance) + command.instanceCount;
            if (end > capacity) throw std::out_of_range("Sable baseInstance/instanceCount exceeds instance buffer");
        } else {
            for (uint32_t i = 0; i < command.indexCount; ++i) {
                const int64_t vertex = static_cast<int64_t>(readIndex(indexBuffer, array.indexType,
                    command.firstIndex + i)) + command.vertexOffset;
                if (vertex < 0 || static_cast<uint64_t>(vertex) >= capacity)
                    throw std::out_of_range("Sable firstIndex/baseVertex exceeds static vertex buffer");
            }
        }
    }
}

void recordDraw(JNIEnv *env, uint32_t arrayId, uint32_t shaderId, uint8_t *uniform,
                uint32_t uniformSize, uint32_t indexCount, uint32_t instanceCount,
                int32_t indirectBufferId, VkDeviceSize indirectOffset,
                uint32_t indirectDrawCount, uint32_t indirectStride) {
    auto framework = Renderer::instance().framework();
    auto buffers = Renderer::instance().buffers();
    if (!framework || !framework->isRunning() || !buffers)
        throw std::runtime_error("Vulkan custom VertexArray backend is unavailable");
    auto pipeline = framework->pipeline();
    if (!pipeline || !pipeline->uiModule()) throw std::runtime_error("Raster pipeline is unavailable");
    NativeVertexArray array = copyArray(arrayId);
    const auto &shader = pipeline->uiModule()->overlayDrawShaderInfo(shaderId);
    validateLayout(array, shader);
    if (array.indexBufferId == UINT32_MAX) throw std::runtime_error("Custom VertexArray has no index buffer");
    if (indexCount > array.indexCount) throw std::out_of_range("Custom VertexArray index count exceeds storage");

    std::vector<CustomVertexBufferBinding> vertexBuffers;
    std::map<uint32_t, std::shared_ptr<vk::DeviceLocalBuffer>> resolvedBindings;
    vertexBuffers.reserve(array.bindings.size());
    for (const auto &[binding, value] : array.bindings) {
        auto buffer = buffers->getBuffer(value.bufferId);
        resolvedBindings.emplace(binding, buffer);
        vertexBuffers.push_back({binding, value.offset, buffer});
    }
    auto indexBuffer = buffers->getBuffer(array.indexBufferId);
    const uint64_t indexBytes = array.indexType == VK_INDEX_TYPE_UINT16 ? 2u : 4u;
    if (static_cast<uint64_t>(array.indexCount) * indexBytes > indexBuffer->size())
        throw std::out_of_range("VertexArray declared index count exceeds actual buffer storage");
    auto indirectBuffer = indirectBufferId < 0 ? nullptr
        : buffers->getBuffer(static_cast<uint32_t>(indirectBufferId));
    if (indirectBuffer) {
        const uint32_t stride = indirectStride == 0 ? sizeof(VkDrawIndexedIndirectCommand)
                                                    : indirectStride;
        const uint64_t requiredBytes = indirectDrawCount == 0 ? 0 :
            static_cast<uint64_t>(indirectDrawCount - 1) * stride + sizeof(VkDrawIndexedIndirectCommand);
        if ((indirectOffset % 4) != 0 || (stride % 4) != 0 ||
            stride < sizeof(VkDrawIndexedIndirectCommand) ||
            indirectOffset > indirectBuffer->size() ||
            requiredBytes > indirectBuffer->size() - indirectOffset)
            throw std::out_of_range("Sable indirect command range exceeds buffer");
        if ((indirectDrawCount > 1 && !framework->device()->hasMultiDrawIndirect()) ||
            indirectDrawCount > framework->physicalDevice()->properties().limits.maxDrawIndirectCount)
            throw std::runtime_error("Indirect draw count exceeds enabled device capabilities");
        const auto *commands = static_cast<const uint8_t *>(indirectBuffer->mappedPtr());
        if (!commands) throw std::runtime_error("Sable indirect staging data is unavailable");
        for (uint32_t i = 0; i < indirectDrawCount; ++i) {
            VkDrawIndexedIndirectCommand command{};
            std::memcpy(&command, commands + indirectOffset + static_cast<uint64_t>(i) * stride,
                        sizeof(command));
            if (command.firstInstance != 0 && !framework->device()->hasDrawIndirectFirstInstance())
                throw std::runtime_error("Nonzero indirect firstInstance requires an enabled device feature");
            validateCommand(array, resolvedBindings, *indexBuffer, command);
        }
    } else {
        VkDrawIndexedIndirectCommand command{indexCount, instanceCount, 0, 0, 0};
        validateCommand(array, resolvedBindings, *indexBuffer, command);
    }
    uint32_t uniformOffset = 0;
    buffers->appendOverlayDrawUniform(uniform, uniformSize, uniformOffset);
    auto context = framework->safeAcquireCurrentContext();
    if (!context) throw std::runtime_error("Custom VertexArray draw requires an acquired frame");
    auto pipelineContext = pipeline->acquirePipelineContext(context);
    if (!pipelineContext || !pipelineContext->uiModuleContext)
        throw std::runtime_error("Custom VertexArray draw context is unavailable");
    pipelineContext->uiModuleContext->drawCustomVertexArray(vertexBuffers, indexBuffer,
        indirectBuffer, shaderId, uniformOffset, indexCount, instanceCount, array.indexType,
        indirectOffset, indirectDrawCount, indirectStride);
}
} // namespace

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_VertexArrayProxy_allocate(
    JNIEnv *env, jclass) {
    jint result = -1;
    jni::invokeVoid(env, "Allocate Vulkan VertexArray", [&] {
        uint32_t id = nextArrayId.fetch_add(1, std::memory_order_relaxed);
        if (id == 0 || id > static_cast<uint32_t>(INT32_MAX))
            throw std::runtime_error("Native VertexArray names exhausted");
        std::lock_guard lock(arraysMutex);
        arrays.emplace(id, NativeVertexArray{});
        result = static_cast<jint>(id);
    });
    return result;
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_VertexArrayProxy_release(
    JNIEnv *env, jclass, jint arrayId) {
    jni::invokeVoid(env, "Release Vulkan VertexArray", [&] {
        if (arrayId < 0) return;
        std::lock_guard lock(arraysMutex);
        arrays.erase(static_cast<uint32_t>(arrayId));
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_VertexArrayProxy_defineVertexBuffer(
    JNIEnv *env, jclass, jint arrayId, jint binding, jint bufferId, jint offset, jint stride,
    jboolean perInstance) {
    jni::invokeVoid(env, "Define Vulkan VertexArray binding", [&] {
        if (arrayId < 0 || binding < 0 || bufferId < 0 || offset < 0 || stride <= 0)
            throw std::invalid_argument("Invalid VertexArray binding");
        std::lock_guard lock(arraysMutex);
        auto &array = arrays.at(static_cast<uint32_t>(arrayId));
        array.bindings[static_cast<uint32_t>(binding)] = {
            static_cast<uint32_t>(bufferId), static_cast<uint32_t>(offset), {
                .binding = static_cast<uint32_t>(binding),
                .stride = static_cast<uint32_t>(stride),
                .inputRate = perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            }};
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_VertexArrayProxy_defineAttribute(
    JNIEnv *env, jclass, jint arrayId, jint location, jint binding, jint componentCount,
    jint componentType, jboolean normalized, jboolean integer, jint relativeOffset) {
    jni::invokeVoid(env, "Define Vulkan VertexArray attribute", [&] {
        if (arrayId < 0 || location < 0 || binding < 0 || relativeOffset < 0)
            throw std::invalid_argument("Invalid VertexArray attribute");
        std::lock_guard lock(arraysMutex);
        auto &array = arrays.at(static_cast<uint32_t>(arrayId));
        array.attributes[static_cast<uint32_t>(location)] = {
            .location = static_cast<uint32_t>(location),
            .binding = static_cast<uint32_t>(binding),
            .format = attributeFormat(componentCount, componentType, normalized, integer),
            .offset = static_cast<uint32_t>(relativeOffset),
        };
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_VertexArrayProxy_removeVertexBuffer(
    JNIEnv *env, jclass, jint arrayId, jint binding) {
    jni::invokeVoid(env, "Remove Vulkan VertexArray binding", [&] {
        std::lock_guard lock(arraysMutex);
        arrays.at(static_cast<uint32_t>(arrayId)).bindings.erase(static_cast<uint32_t>(binding));
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_VertexArrayProxy_removeAttribute(
    JNIEnv *env, jclass, jint arrayId, jint location) {
    jni::invokeVoid(env, "Remove Vulkan VertexArray attribute", [&] {
        std::lock_guard lock(arraysMutex);
        arrays.at(static_cast<uint32_t>(arrayId)).attributes.erase(static_cast<uint32_t>(location));
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_VertexArrayProxy_clearVertexBuffers(
    JNIEnv *env, jclass, jint arrayId) {
    jni::invokeVoid(env, "Clear Vulkan VertexArray bindings", [&] {
        std::lock_guard lock(arraysMutex);
        arrays.at(static_cast<uint32_t>(arrayId)).bindings.clear();
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_VertexArrayProxy_clearAttributes(
    JNIEnv *env, jclass, jint arrayId) {
    jni::invokeVoid(env, "Clear Vulkan VertexArray attributes", [&] {
        std::lock_guard lock(arraysMutex);
        arrays.at(static_cast<uint32_t>(arrayId)).attributes.clear();
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_VertexArrayProxy_defineIndexBuffer(
    JNIEnv *env, jclass, jint arrayId, jint bufferId, jint indexType, jint indexCount) {
    jni::invokeVoid(env, "Define Vulkan VertexArray index buffer", [&] {
        if (arrayId < 0 || bufferId < 0 || indexCount < 0 || (indexType != 0 && indexType != 1))
            throw std::invalid_argument("Invalid VertexArray index buffer");
        std::lock_guard lock(arraysMutex);
        auto &array = arrays.at(static_cast<uint32_t>(arrayId));
        array.indexBufferId = static_cast<uint32_t>(bufferId);
        array.indexType = indexType == 0 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        array.indexCount = static_cast<uint32_t>(indexCount);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_VertexArrayProxy_draw(
    JNIEnv *env, jclass, jint arrayId, jint shaderId, jint indexCount, jint instanceCount,
    jlong uniformPtr, jint uniformSize) {
    jni::invokeVoid(env, "Draw Vulkan VertexArray", [&] {
        if (arrayId < 0 || shaderId < 0 || indexCount < 0 || instanceCount < 0 ||
            uniformSize < 0 || (uniformSize > 0 && uniformPtr == 0))
            throw std::invalid_argument("Invalid direct VertexArray draw");
        recordDraw(env, static_cast<uint32_t>(arrayId), static_cast<uint32_t>(shaderId),
            reinterpret_cast<uint8_t *>(uniformPtr), static_cast<uint32_t>(uniformSize),
            static_cast<uint32_t>(indexCount), static_cast<uint32_t>(instanceCount),
            -1, 0, 0, 0);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_VertexArrayProxy_drawIndirect(
    JNIEnv *env, jclass, jint arrayId, jint shaderId, jint indirectBufferId,
    jlong indirectOffset, jint drawCount, jint stride, jlong uniformPtr, jint uniformSize) {
    jni::invokeVoid(env, "Draw indirect Vulkan VertexArray", [&] {
        if (arrayId < 0 || shaderId < 0 || indirectBufferId < 0 || indirectOffset < 0 ||
            drawCount < 0 || stride < 0 || uniformSize < 0 || (uniformSize > 0 && uniformPtr == 0))
            throw std::invalid_argument("Invalid indirect VertexArray draw");
        NativeVertexArray array = copyArray(static_cast<uint32_t>(arrayId));
        recordDraw(env, static_cast<uint32_t>(arrayId), static_cast<uint32_t>(shaderId),
            reinterpret_cast<uint8_t *>(uniformPtr), static_cast<uint32_t>(uniformSize),
            array.indexCount, 0, indirectBufferId, static_cast<VkDeviceSize>(indirectOffset),
            static_cast<uint32_t>(drawCount), static_cast<uint32_t>(stride));
    });
}
