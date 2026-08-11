#include "com_radiance_client_proxy_vulkan_TextureProxy.h"

#include "core/render/emission.hpp"
#include "core/render/renderer.hpp"
#include "core/render/textures.hpp"
#include "core/middleware/jni_exception.hpp"

extern "C" {
JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_generateTextureIdNative(JNIEnv *env, jclass) {
    jint id = 0;
    jni::invokeVoid(env, "Allocate Vulkan texture ID", [&] {
        auto textures = Renderer::instance().textures();
        if (textures == nullptr) throw std::runtime_error("Vulkan textures are not initialized");
        id = static_cast<jint>(textures->allocateTexture());
    });
    return id;
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_releaseTextureIdNative(
    JNIEnv *env, jclass, jint id, jint fallbackId) {
    jni::invokeVoid(env, "Release Vulkan texture ID", [&] {
        auto textures = Renderer::instance().textures();
        if (textures != nullptr) {
            textures->releaseTexture(static_cast<uint32_t>(id), static_cast<uint32_t>(fallbackId));
        }
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_prepareImageNative(
    JNIEnv *env, jclass, jint id, jint maxLevel, jint width, jint height, jint format) {
    jni::invokeVoid(env, "Allocate Vulkan texture", [&] {
        auto textures = Renderer::instance().textures();
        if (textures == nullptr) return;
        auto vkFormat = static_cast<VkFormat>(format);
        textures->initializeTexture(id, maxLevel, width, height, vkFormat);
        if (auto emission = textures->emission(); emission != nullptr) {
            emission->resetTexture(static_cast<uint32_t>(id));
        }
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_setFilter(
    JNIEnv *env, jclass, jint id, jint samplingMode, jint mipmapMode) {
    jni::invokeVoid(env, "Configure Vulkan texture filtering", [&] {
        auto textures = Renderer::instance().textures();
        if (textures == nullptr) return;
        auto vkSamplingMode = static_cast<VkFilter>(samplingMode);
        auto vkMipmapMode = static_cast<VkSamplerMipmapMode>(mipmapMode);
        textures->setSamplingMode(id, vkSamplingMode, vkMipmapMode);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_setClamp(JNIEnv *env,
                                                                                   jclass,
                                                                                   jint id,
                                                                                   jint addressMode) {
    jni::invokeVoid(env, "Configure Vulkan texture addressing", [&] {
        auto textures = Renderer::instance().textures();
        if (textures == nullptr) return;
        auto vkSamplerAddressMode = static_cast<VkSamplerAddressMode>(addressMode);
        textures->setAddressMode(id, vkSamplerAddressMode);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_queueUpload(JNIEnv *env,
                                                                                      jclass,
                                                                                      jlong srcPointer,
                                                                                      jint srcSizeInBytes,
                                                                                      jint srcRowPixels,
                                                                                      jint dstId,
                                                                                      jint srcOffsetX,
                                                                                      jint srcOffsetY,
                                                                                      jint dstOffsetX,
                                                                                      jint dstOffsetY,
                                                                                      jint width,
                                                                                      jint height,
                                                                                      jint level) {
    jni::invokeVoid(env, "Queue Vulkan texture upload", [&] {
        auto textures = Renderer::instance().textures();
        if (textures == nullptr) return;
        textures->queueUpload(reinterpret_cast<uint8_t *>(srcPointer), srcSizeInBytes, srcRowPixels, dstId, srcOffsetX,
                              srcOffsetY, dstOffsetX, dstOffsetY, width, height, level);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_downloadTexture(
    JNIEnv *env, jclass, jint id, jint level, jint width, jint height, jint channel, jlong dstPointer) {
    jni::invokeVoid(env, "Download Vulkan texture", [&] {
        auto textures = Renderer::instance().textures();
        if (textures == nullptr) { throw std::runtime_error("Texture manager is unavailable"); }
        const VkResult result = textures->downloadTexture(static_cast<uint32_t>(id), static_cast<uint32_t>(level),
                                                          static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                                          static_cast<uint32_t>(channel),
                                                          reinterpret_cast<void *>(dstPointer));
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Texture download failed with VkResult=" + std::to_string(result));
        }
    });
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_isFramebufferTexture(
    JNIEnv *env, jclass, jint id) {
    jboolean result = JNI_FALSE;
    jni::invokeVoid(env, "Query framebuffer texture storage", [&] {
        auto textures = Renderer::instance().textures();
        if (!textures) throw std::runtime_error("Texture manager is unavailable");
        result = textures->isFramebufferTexture(id) ? JNI_TRUE : JNI_FALSE;
    });
    return result;
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_uploadEmissionTileNative(JNIEnv *env,
                                                                                                   jclass,
                                                                                                   jint textureId,
                                                                                                   jlong tileKey,
                                                                                                   jlong cellsPtr,
                                                                                                   jint cellCount) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.TextureProxy.uploadEmissionTileNative", [&] {
            auto textures = Renderer::instance().textures();
            if (textures == nullptr) return;
            auto emission = textures->emission();
            if (emission == nullptr) return;

            emission->updateTile(static_cast<uint32_t>(textureId), static_cast<uint64_t>(tileKey),
                                 reinterpret_cast<const EmissionCellUpload *>(cellsPtr), cellCount);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_performQueuedUpload(JNIEnv *env, jclass) {
    jni::invokeVoid(env, "Upload Vulkan textures", [&] {
        auto textures = Renderer::instance().textures();
        if (textures == nullptr) return;
        textures->performQueuedUpload();
    });
}
}
