#include "core/render/chunk_trace.hpp"
#include "com_radiance_client_proxy_world_ChunkProxy.h"

#include "core/render/chunks.hpp"
#include "core/render/renderer.hpp"
#include "core/middleware/jni_exception.hpp"

#include <iostream>

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_initNative(JNIEnv *env,
                                                                                  jclass,
                                                                                  jint chunkNum,
                                                                                  jint sizeX,
                                                                                  jint sizeY,
                                                                                  jint sizeZ,
                                                                                  jint bottomSectionCoord) {
    jni::invokeVoid(env, "Initialize chunk storage", [&] {
        auto world = Renderer::instance().world();
        if (world != nullptr) { world->chunks()->reset(chunkNum, sizeX, sizeY, sizeZ, bottomSectionCoord); }
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_updateSectionPosNative(JNIEnv *env,
                                                                                              jclass,
                                                                                              jint sectionX,
                                                                                              jint sectionY,
                                                                                              jint sectionZ) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.world.ChunkProxy.updateSectionPosNative", [&] {
            auto world = Renderer::instance().world();
            if (world == nullptr) return;
            world->chunks()->setChunkStorageSectionPos(glm::ivec3(sectionX, sectionY, sectionZ));
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_rebuildSingle(JNIEnv *env,
                                                                                     jclass,
                                                                                     jint originX,
                                                                                     jint originY,
                                                                                     jint originZ,
                                                                                     jlong index,
                                                                                     jlong generation,
                                                                                     jint geometryCount,
                                                                                      jlong geometryTypes,
                                                                                      jlong geometryGroupNames,
                                                                                      jlong geometryMaterialFlags,
                                                                                      jlong geometryTextures,
                                                                                     jlong vertexFormats,
                                                                                     jlong vertexCounts,
                                                                                     jlong vertexAddrs,
                                                                                     jboolean important,
                                                                                     jint priority,
                                                                                     jboolean collectEmission) {
    jni::invokeVoid(env, "Queue chunk rebuild", [&] {
        if (priority < 0 || priority > 1) throw std::invalid_argument("Chunk priority outside [0,1]");
        auto world = Renderer::instance().world();
        if (world == nullptr) return;
        world->chunks()->queueChunkBuild(ChunkBuildTask{
            .x = originX,
            .y = originY,
            .z = originZ,
            .id = index,
            .generation = generation,
            .geometryCount = geometryCount,
            .geometryTypes = reinterpret_cast<int *>(geometryTypes),
            .geometryGroupNames = reinterpret_cast<const char **>(geometryGroupNames),
            .geometryMaterialFlags = reinterpret_cast<int *>(geometryMaterialFlags),
            .geometryTextures = reinterpret_cast<int *>(geometryTextures),
            .vertexFormats = reinterpret_cast<int *>(vertexFormats),
            .vertexCounts = reinterpret_cast<int *>(vertexCounts),
            .vertices = reinterpret_cast<vk::VertexFormat::PBRVertex **>(vertexAddrs),
            .isImportant = static_cast<bool>(important),
            .priority = priority,
            .collectEmission = static_cast<bool>(collectEmission),
        });
    });
}

JNIEXPORT jlong JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_allocateExternalChunkNative(JNIEnv *env,
                                                                                                     jclass) {
    return jni::invoke<jlong>(env, "JNI com.radiance.client.proxy.world.ChunkProxy.allocateExternalChunkNative", 0, [&]() -> jlong {
            auto world = Renderer::instance().world();
            if (world == nullptr) return -1;
            return world->chunks()->allocateExternalChunk();
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_updateExternalChunkTransformNative(
    JNIEnv *env, jclass, jlong index,
    jdouble m00, jdouble m01, jdouble m02, jdouble m03,
    jdouble m10, jdouble m11, jdouble m12, jdouble m13,
    jdouble m20, jdouble m21, jdouble m22, jdouble m23) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.world.ChunkProxy.updateExternalChunkTransformNative", [&] {
            auto world = Renderer::instance().world();
            if (world == nullptr) return;

            glm::dmat4 transform(1.0);
            transform[0] = glm::dvec4(m00, m10, m20, 0.0);
            transform[1] = glm::dvec4(m01, m11, m21, 0.0);
            transform[2] = glm::dvec4(m02, m12, m22, 0.0);
            transform[3] = glm::dvec4(m03, m13, m23, 1.0);
            world->chunks()->updateExternalChunkTransform(index, transform);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_releaseExternalChunkNative(JNIEnv *env,
                                                                                                    jclass,
                                                                                                    jlong index) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.world.ChunkProxy.releaseExternalChunkNative", [&] {
            auto world = Renderer::instance().world();
            if (world == nullptr) return;
            world->chunks()->releaseExternalChunk(index);
    });
}

JNIEXPORT jboolean JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_isChunkReady(JNIEnv *env, jclass, jlong id) {
    return jni::invoke<jboolean>(env, "JNI com.radiance.client.proxy.world.ChunkProxy.isChunkReady", JNI_FALSE, [&]() -> jboolean {
            auto world = Renderer::instance().world();
            if (world == nullptr)
                return false;
            else
                return world->chunks()->isChunkReady(id);
    });
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_countReadyChunksNative(JNIEnv *env, jclass) {
    return jni::invoke<jint>(env, "JNI com.radiance.client.proxy.world.ChunkProxy.countReadyChunksNative", 0, [&]() -> jint {
            auto world = Renderer::instance().world();
            if (world == nullptr || world->chunks() == nullptr) return 0;
            return static_cast<jint>(world->chunks()->countReadyPrimaryChunks());
    });
}

JNIEXPORT jstring JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_performanceSnapshotNative(JNIEnv *env,
                                                                                                     jclass) {
    return jni::invoke<jstring>(env, "JNI com.radiance.client.proxy.world.ChunkProxy.performanceSnapshotNative", nullptr, [&]() -> jstring {
            auto world = Renderer::instance().world();
            if (world == nullptr || world->chunks() == nullptr) return env->NewStringUTF("");
            const std::string snapshot = world->chunks()->performanceSnapshot();
            return env->NewStringUTF(snapshot.c_str());
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_markChunkDirtyNative(JNIEnv *env,
                                                                                             jclass,
                                                                                             jlong index,
                                                                                             jlong generation) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.world.ChunkProxy.markChunkDirtyNative", [&] {
            auto world = Renderer::instance().world();
            if (world == nullptr) return;
            world->chunks()->markChunkDirty(index, generation);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_relocateSingle(JNIEnv *env,
                                                                                      jclass,
                                                                                      jlong index,
                                                                                      jint originX,
                                                                                      jint originY,
                                                                                      jint originZ,
                                                                                      jlong generation) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.world.ChunkProxy.relocateSingle", [&] {
            auto world = Renderer::instance().world();
            if (world == nullptr) return;
            world->chunks()->relocateChunk(index, originX, originY, originZ, generation);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_invalidateSingle(JNIEnv *env,
                                                                                        jclass,
                                                                                        jlong index,
                                                                                        jlong generation) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.world.ChunkProxy.invalidateSingle", [&] {
            auto world = Renderer::instance().world();
            if (world == nullptr) return;
            world->chunks()->invalidateChunk(index, generation);
    });
}

JNIEXPORT jstring JNICALL Java_com_radiance_client_proxy_world_ChunkProxy_drainUpdateTraceNative(JNIEnv* env,jclass) {
    return jni::invoke<jstring>(env,"Read chunk update trace",nullptr,[&]() -> jstring {
        auto data=mcvr::chunkTrace::drain();
        return env->NewStringUTF(data.c_str());
    }, jni::BoundaryPolicy::allowAfterFatal);
}
