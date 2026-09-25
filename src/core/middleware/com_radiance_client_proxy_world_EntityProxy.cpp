#include "com_radiance_client_proxy_world_EntityProxy.h"

#include "core/middleware/jni_exception.hpp"
#include "core/render/entities.hpp"
#include "core/render/renderer.hpp"

extern "C" JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_EntityProxy_queueRigidModel(
    JNIEnv *env, jclass, jlong model, jlong instance, jint type, jint texture,
    jlong vertices, jint count, jdouble x, jdouble y, jdouble z, jint mask, jlong matrix, jlong group) {
    jni::invokeVoid(env, "Queue rigid model", [&] {
        auto world=Renderer::instance().world();
        if (!world) throw std::logic_error("Rigid model submitted without a world");
        world->entities()->queueRigidModel(model,instance,type,texture,reinterpret_cast<const void *>(vertices),
            count,x,y,z,mask,reinterpret_cast<const float *>(matrix),reinterpret_cast<const char *>(group));
    });
}

JNIEXPORT jboolean JNICALL Java_com_radiance_client_proxy_world_EntityProxy_beginCachedCloud(
    JNIEnv *env, jclass, jlong revision, jdouble x, jdouble y, jdouble z) {
    return jni::invoke<jboolean>(env, "Queue cached cloud geometry", JNI_FALSE, [&]() -> jboolean {
        auto world = Renderer::instance().world();
        if (!world) throw std::logic_error("Cloud capture without a world");
        return world->entities()->beginCachedCloud(static_cast<uint64_t>(revision), x, y, z) ? JNI_TRUE : JNI_FALSE;
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_EntityProxy_endCachedCloud(
    JNIEnv *env, jclass, jboolean success) {
    jni::invokeVoid(env, "Finish cached cloud geometry", [&] {
        auto world = Renderer::instance().world();
        if (!world) throw std::logic_error("Cloud capture without a world");
        world->entities()->endCachedCloud(success == JNI_TRUE);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_EntityProxy_queueBuild(JNIEnv *env,
                                                                                   jclass,
                                                                                   jfloat lineWidth,
                                                                                   jint coordinate,
                                                                                   jboolean normalOffset,
                                                                                   jint size,
                                                                                   jlong entityHashCodes,
                                                                                   jlong entityPosXs,
                                                                                   jlong entityPosYs,
                                                                                   jlong entityPosZs,
                                                                                   jlong entityRayTracingFlags,
                                                                                   jlong entityPostRenderFlags,
                                                                                   jlong entityPrebuiltBLASs,
                                                                                   jlong entityPosts,
                                                                                   jlong entityLayerCounts,
                                                                                   jlong entityLineFrames,
                                                                                   jlong geometryTypes,
                                                                                   jlong geometryGroupNames,
                                                                                   jlong geometryContentNames,
                                                                                   jlong geometryTextures,
                                                                                   jlong vertexFormats,
                                                                                   jlong indexFormats,
                                                                                   jlong vertexCounts,
                                                                                   jlong vertices) {
    jni::invokeVoid(env, "Queue entity geometry build", [&] {
        auto world = Renderer::instance().world();
        if (world == nullptr) return;
        world->entities()->queueBuild(EntitiesBuildTask{
            .lineWidth = lineWidth,
            .coordinate = static_cast<World::Coordinates>(coordinate),
            .normalOffset = static_cast<bool>(normalOffset),
            .entityCount = size,
            .entityHashCodes = reinterpret_cast<int *>(entityHashCodes),
            .entityXs = reinterpret_cast<double *>(entityPosXs),
            .entityYs = reinterpret_cast<double *>(entityPosYs),
            .entityZs = reinterpret_cast<double *>(entityPosZs),
            .entityRayTracingFlags = reinterpret_cast<int *>(entityRayTracingFlags),
            .entityPostRenderFlags = reinterpret_cast<int *>(entityPostRenderFlags),
            .entityPrebuiltBLASs = reinterpret_cast<int *>(entityPrebuiltBLASs),
            .entityPosts = reinterpret_cast<int *>(entityPosts),
            .entityGeometryCounts = reinterpret_cast<int *>(entityLayerCounts),
            .entityLineFrames = reinterpret_cast<const float *>(entityLineFrames),
            .geometryTypes = reinterpret_cast<int *>(geometryTypes),
            .geometryGroupNames = reinterpret_cast<const char **>(geometryGroupNames),
            .geometryContentNames = reinterpret_cast<const char **>(geometryContentNames),
            .geometryTextures = reinterpret_cast<int *>(geometryTextures),
            .vertexFormats = reinterpret_cast<int *>(vertexFormats),
            .indexFormats = reinterpret_cast<int *>(indexFormats),
            .vertexCounts = reinterpret_cast<int *>(vertexCounts),
            .vertices = reinterpret_cast<void **>(vertices),
        });
    });
}

extern "C" JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_world_EntityProxy_beginWorldMeshFrame(
    JNIEnv *env, jclass, jlong worldToken, jlong frameToken, jlong resourceGeneration) {
    return jni::invokeForVkResult(env, "Begin world mesh frame", [&] {
        auto world = Renderer::instance().world();
        if (world == nullptr) return 2;
        return world->entities()->beginWorldMeshFrame(static_cast<uint64_t>(worldToken),
            static_cast<uint64_t>(frameToken), static_cast<uint64_t>(resourceGeneration));
    });
}

extern "C" JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_world_EntityProxy_beginWorldMeshStage(
    JNIEnv *env, jclass, jlong worldToken, jlong frameToken, jlong resourceGeneration,
    jlong stageToken, jint stage) {
    return jni::invokeForVkResult(env, "Begin world mesh stage", [&] {
        auto world = Renderer::instance().world();
        if (world == nullptr) return 2;
        return world->entities()->beginWorldMeshStage(static_cast<uint64_t>(worldToken),
            static_cast<uint64_t>(frameToken), static_cast<uint64_t>(resourceGeneration),
            static_cast<uint64_t>(stageToken), stage);
    });
}

extern "C" JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_world_EntityProxy_queueWorldMesh(
    JNIEnv *env, jclass, jlong worldToken, jlong frameToken, jlong resourceGeneration,
    jlong stageToken, jint stage, jint coordinate, jdouble originX, jdouble originY,
    jdouble originZ, jint sourceId, jint rayTracingFlag, jint geometryType, jint textureId,
    jint vertexFormat, jint drawMode, jint indexType, jint vertexCount, jint indexCount,
    jlong vertices, jint vertexBytes, jlong indices, jint indexBytes, jint alphaMode,
    jfloat emission, jlong shaderKey, jlong materialKey, jlong auditId) {
    return jni::invokeForVkResult(env, "Queue world mesh", [&] {
        auto world = Renderer::instance().world();
        if (world == nullptr) return 2;
        return world->entities()->queueWorldMesh(WorldMeshBuildTask{
            .worldToken = static_cast<uint64_t>(worldToken),
            .frameToken = static_cast<uint64_t>(frameToken),
            .resourceGeneration = static_cast<uint64_t>(resourceGeneration),
            .stageToken = static_cast<uint64_t>(stageToken),
            .stage = stage,
            .coordinate = static_cast<World::Coordinates>(coordinate),
            .originX = originX,
            .originY = originY,
            .originZ = originZ,
            .sourceId = sourceId,
            .rayTracingFlag = rayTracingFlag,
            .geometryType = geometryType,
            .textureId = textureId,
            .vertexFormat = vertexFormat,
            .drawMode = drawMode,
            .indexType = indexType,
            .vertexCount = vertexCount,
            .indexCount = indexCount,
            .vertices = reinterpret_cast<void *>(vertices),
            .vertexBytes = vertexBytes,
            .indices = reinterpret_cast<void *>(indices),
            .indexBytes = indexBytes,
            .alphaMode = alphaMode,
            .emission = emission,
            .shaderKey = reinterpret_cast<const char *>(shaderKey),
            .materialKey = reinterpret_cast<const char *>(materialKey),
            .auditId = static_cast<uint64_t>(auditId),
        });
    });
}

extern "C" JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_world_EntityProxy_pollWorldMeshAudit(
    JNIEnv *env, jclass, jlong auditId, jboolean consumeTerminal) {
    return jni::invokeForVkResult(env, "Poll world mesh audit", [&] {
        auto world = Renderer::instance().world();
        if (world == nullptr) return 0;
        return world->entities()->pollWorldMeshAudit(static_cast<uint64_t>(auditId),
            static_cast<bool>(consumeTerminal));
    });
}

extern "C" JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_EntityProxy_endWorldMeshStage(
    JNIEnv *env, jclass, jlong worldToken, jlong frameToken, jlong resourceGeneration,
    jlong stageToken, jboolean commit) {
    jni::invokeVoid(env, "End world mesh stage", [&] {
        auto world = Renderer::instance().world();
        if (world == nullptr) return;
        world->entities()->endWorldMeshStage(static_cast<uint64_t>(worldToken),
            static_cast<uint64_t>(frameToken), static_cast<uint64_t>(resourceGeneration),
            static_cast<uint64_t>(stageToken), static_cast<bool>(commit));
    });
}

extern "C" JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_EntityProxy_endWorldMeshFrame(
    JNIEnv *env, jclass, jlong worldToken, jlong frameToken, jlong resourceGeneration,
    jboolean commit) {
    jni::invokeVoid(env, "End world mesh frame", [&] {
        auto world = Renderer::instance().world();
        if (world == nullptr) return;
        world->entities()->endWorldMeshFrame(static_cast<uint64_t>(worldToken),
            static_cast<uint64_t>(frameToken), static_cast<uint64_t>(resourceGeneration),
            static_cast<bool>(commit));
    });
}

extern "C" JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_EntityProxy_invalidateWorldMeshGeneration(
    JNIEnv *env, jclass, jlong resourceGeneration) {
    jni::invokeVoid(env, "Invalidate world mesh generation", [&] {
        auto world = Renderer::instance().world();
        if (world == nullptr) return;
        world->entities()->invalidateWorldMeshGeneration(static_cast<uint64_t>(resourceGeneration));
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_EntityProxy_build(JNIEnv *env, jclass) {
    jni::invokeVoid(env, "Build entity geometry", [] {
        auto world = Renderer::instance().world();
        if (world == nullptr) return;
        world->entities()->build();
    });
}
