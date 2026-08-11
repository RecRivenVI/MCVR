#include "com_radiance_client_proxy_vulkan_ShaderProxy.h"

#include "core/all_extern.hpp"
#include "core/diagnostics/draw_state_trace.hpp"
#include "core/render/buffers.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/middleware/jni_exception.hpp"
#include "core/middleware/jni_string.hpp"

#include <algorithm>
#include <optional>
#include <unordered_map>

namespace {
std::string toStdString(JNIEnv *env, jstring value) {
    if (value == nullptr) return {};
    auto result = jni::copyUtf8(env, value);
    if (!result) throw std::runtime_error("Cannot read shader string");
    return std::move(*result);
}

VkFormat customAttributeFormat(int components, int type, bool normalized, bool integer) {
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

std::optional<vk::VertexLayoutInfo> parseCustomVertexLayout(JNIEnv *env,
                                                             jintArray bindingArray,
                                                             jintArray attributeArray) {
    if (bindingArray == nullptr && attributeArray == nullptr) return std::nullopt;
    if (bindingArray == nullptr || attributeArray == nullptr)
        throw std::invalid_argument("Custom vertex bindings and attributes must be paired");
    const jsize bindingLength = env->GetArrayLength(bindingArray);
    const jsize attributeLength = env->GetArrayLength(attributeArray);
    if (bindingLength == 0 || bindingLength % 3 != 0 ||
        attributeLength == 0 || attributeLength % 7 != 0)
        throw std::invalid_argument("Malformed custom vertex layout payload");
    jint *bindings = env->GetIntArrayElements(bindingArray, nullptr);
    jint *attributes = env->GetIntArrayElements(attributeArray, nullptr);
    if (!bindings || !attributes) {
        if (bindings) env->ReleaseIntArrayElements(bindingArray, bindings, JNI_ABORT);
        if (attributes) env->ReleaseIntArrayElements(attributeArray, attributes, JNI_ABORT);
        throw std::bad_alloc();
    }
    vk::VertexLayoutInfo layout{};
    try {
        for (jsize i = 0; i < bindingLength; i += 3) {
            if (bindings[i] < 0 || bindings[i + 1] <= 0 ||
                (bindings[i + 2] != 0 && bindings[i + 2] != 1))
                throw std::invalid_argument("Invalid custom vertex binding");
            layout.bindingDescriptions.push_back({
                .binding = static_cast<uint32_t>(bindings[i]),
                .stride = static_cast<uint32_t>(bindings[i + 1]),
                .inputRate = bindings[i + 2] == 0 ? VK_VERTEX_INPUT_RATE_VERTEX
                                                   : VK_VERTEX_INPUT_RATE_INSTANCE,
            });
        }
        for (jsize i = 0; i < attributeLength; i += 7) {
            if (attributes[i] < 0 || attributes[i + 1] < 0 || attributes[i + 6] < 0)
                throw std::invalid_argument("Invalid custom vertex attribute");
            layout.attributeDescriptions.push_back({
                .location = static_cast<uint32_t>(attributes[i]),
                .binding = static_cast<uint32_t>(attributes[i + 1]),
                .format = customAttributeFormat(attributes[i + 2], attributes[i + 3],
                    attributes[i + 4] != 0, attributes[i + 5] != 0),
                .offset = static_cast<uint32_t>(attributes[i + 6]),
            });
        }
    } catch (...) {
        env->ReleaseIntArrayElements(bindingArray, bindings, JNI_ABORT);
        env->ReleaseIntArrayElements(attributeArray, attributes, JNI_ABORT);
        throw;
    }
    env->ReleaseIntArrayElements(bindingArray, bindings, JNI_ABORT);
    env->ReleaseIntArrayElements(attributeArray, attributes, JNI_ABORT);
    return layout;
}
} // namespace

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_ShaderProxy_registerShader(
    JNIEnv *env,
    jclass,
    jstring shaderKey,
    jint vertexFormatType,
    jint drawMode,
    jint uniformSize,
    jstring vertexShaderPath,
    jstring fragmentShaderPath,
    jstring tessellationControlShaderPath,
    jstring tessellationEvaluationShaderPath,
    jint patchControlPoints,
    jintArray customVertexBindings,
    jintArray customVertexAttributes,
    jobjectArray defineNames,
    jobjectArray defineValues) {
    jint result = -1;
    jni::invokeVoid(env, "Register Vulkan shader", [&] {
        auto framework = Renderer::instance().framework();
        if (framework == nullptr) throw std::runtime_error("Renderer is unavailable");
        std::lock_guard lock(framework->recreateMtx());

        std::unordered_map<std::string, std::string> definitions;
        if (defineNames != nullptr && defineValues != nullptr) {
            jsize count = std::min(env->GetArrayLength(defineNames), env->GetArrayLength(defineValues));
            for (jsize i = 0; i < count; ++i) {
                jni::LocalRef name(env, static_cast<jstring>(env->GetObjectArrayElement(defineNames, i)));
                if (env->ExceptionCheck()) throw std::runtime_error("Cannot read shader define name");
                jni::LocalRef value(env, static_cast<jstring>(env->GetObjectArrayElement(defineValues, i)));
                if (env->ExceptionCheck()) throw std::runtime_error("Cannot read shader define value");
                definitions.emplace(toStdString(env, name.get()), toStdString(env, value.get()));
            }
        }

        auto shaderId = framework->pipeline()->uiModule()->registerOverlayDrawShader(
            toStdString(env, shaderKey), vertexFormatType, drawMode, uniformSize,
            toStdString(env, vertexShaderPath), toStdString(env, fragmentShaderPath),
            toStdString(env, tessellationControlShaderPath),
            toStdString(env, tessellationEvaluationShaderPath), patchControlPoints,
            parseCustomVertexLayout(env, customVertexBindings, customVertexAttributes), definitions);
        result = static_cast<jint>(shaderId);
    });
    return result;
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_ShaderProxy_draw(
    JNIEnv *env, jclass, jint vertexId, jint indexId, jint patchIndexId, jint shaderId,
    jint indexCount, jint patchIndexCount, jint indexType, jlong uniformPtr, jint uniformSize) {
    jni::invokeVoid(env, "Record indexed Vulkan raster draw", [&] {
        if (vertexId < 0 || indexId < 0) throw std::invalid_argument("Negative raster buffer id");
        if (shaderId < 0) throw std::invalid_argument("Negative raster shader id");
        if (indexCount < 0) throw std::invalid_argument("Negative raster index count");
        if (patchIndexCount < 0) throw std::invalid_argument("Negative patch index count");
        if ((patchIndexId < 0) != (patchIndexCount == 0))
            throw std::invalid_argument("Patch index id/count mismatch");
        if (indexType != VK_INDEX_TYPE_UINT16 && indexType != VK_INDEX_TYPE_UINT32)
            throw std::invalid_argument("Unsupported raster index type");
        if (uniformSize < 0) throw std::invalid_argument("Negative raster uniform size");
        if (uniformSize > 0 && uniformPtr == 0) throw std::invalid_argument("Null raster uniform data");

        auto framework = Renderer::instance().framework();
        auto buffers = Renderer::instance().buffers();
        if (framework == nullptr || !framework->isRunning()) throw std::runtime_error("Renderer is unavailable");
        if (buffers == nullptr) throw std::runtime_error("Vulkan buffer manager is not initialized");
        auto pipeline = framework->pipeline();
        if (pipeline == nullptr || pipeline->uiModule() == nullptr)
            throw std::runtime_error("Raster pipeline is unavailable");

        auto vertexBuffer = buffers->getBuffer(vertexId);
        auto indexBuffer = buffers->getBuffer(indexId);
        auto patchIndexBuffer = patchIndexId < 0 ? nullptr : buffers->getBuffer(patchIndexId);
        uint32_t uniformOffset = 0;
        buffers->appendOverlayDrawUniform(reinterpret_cast<uint8_t *>(uniformPtr), uniformSize, uniformOffset);
        auto context = framework->safeAcquireCurrentContext();
        if (context == nullptr) throw std::runtime_error("Raster draw requires an acquired frame");
        mcvr::diagnostics::recordUniform(
            "ShaderProxy.draw", context->frameIndex, context->frameSubmitted,
            static_cast<uint32_t>(shaderId), static_cast<uint32_t>(vertexId),
            static_cast<uint32_t>(indexId), static_cast<int32_t>(patchIndexId),
            uniformOffset, reinterpret_cast<const uint8_t *>(uniformPtr),
            static_cast<size_t>(uniformSize));
        auto pipelineContext = pipeline->acquirePipelineContext(context);
        if (pipelineContext == nullptr || pipelineContext->uiModuleContext == nullptr)
            throw std::runtime_error("Raster draw context is unavailable");
        pipelineContext->uiModuleContext->drawIndexed(
            vertexBuffer, indexBuffer, patchIndexBuffer, static_cast<uint32_t>(shaderId),
            uniformOffset, static_cast<uint32_t>(indexCount),
            static_cast<uint32_t>(patchIndexCount), static_cast<VkIndexType>(indexType));
    });
}
