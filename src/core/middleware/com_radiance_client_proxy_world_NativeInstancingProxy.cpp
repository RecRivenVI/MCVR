#include "com_radiance_client_proxy_world_NativeInstancingProxy.h"

#include "core/middleware/jni_exception.hpp"
#include "core/middleware/jni_string.hpp"
#include "core/render/instancing.hpp"
#include "core/render/renderer.hpp"
#include "core/render/world.hpp"

namespace {
std::shared_ptr<Instancing> instancing() {
    auto world = Renderer::instance().world();
    if (world == nullptr || world->instancing() == nullptr) throw std::runtime_error("Native world instancing is unavailable");
    return world->instancing();
}
}

JNIEXPORT jboolean JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_isSupported(JNIEnv *env, jclass) {
    return jni::invoke<jboolean>(env, "Query Flywheel instancing support", JNI_FALSE, []() -> jboolean {
        return Renderer::is_initialized() && Renderer::instance().world() != nullptr ? JNI_TRUE : JNI_FALSE;
    });
}
JNIEXPORT jlong JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_createEngine(JNIEnv *env, jclass) {
    jlong result = 0; jni::invokeVoid(env, "Create Flywheel engine", [&] { result = static_cast<jlong>(instancing()->createEngine()); }); return result;
}
JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_createModel(JNIEnv *env, jclass, jlong engine, jint count) {
    jint result = 0; jni::invokeVoid(env, "Create Flywheel model", [&] { result = instancing()->engine(static_cast<uint64_t>(engine))->createModel(count); }); return result;
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_uploadModelMesh(
    JNIEnv *env, jclass, jlong engine, jint model, jint mesh, jlong vertices, jint vertexCount,
    jlong indices, jint indexCount, jint texture, jint alpha, jint flags, jstring key) {
    jni::invokeVoid(env, "Upload Flywheel model mesh", [&] {
        const auto material = key == nullptr ? std::optional<std::string>(std::string{}) : jni::copyUtf8(env, key);
        if (!material) throw std::runtime_error("Cannot read Flywheel material key");
        instancing()->engine(static_cast<uint64_t>(engine))->uploadModelMesh(model, mesh, reinterpret_cast<const void *>(vertices), vertexCount,
            reinterpret_cast<const uint32_t *>(indices), indexCount, texture, alpha, flags, material->c_str());
    });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_finishModel(JNIEnv *env, jclass, jlong engine, jint model) {
    jni::invokeVoid(env, "Finish Flywheel model", [&] { instancing()->engine(static_cast<uint64_t>(engine))->finishModel(model); });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_updateInstance(
    JNIEnv *env, jclass, jlong engine, jlong id, jint model, jint adapter, jint bias, jboolean visible,
    jlong data, jint size, jlong pose, jlong normal, jboolean embedded) {
    jni::invokeVoid(env, "Update Flywheel instance", [&] {
        instancing()->engine(static_cast<uint64_t>(engine))->updateInstance(id, model, adapter, bias, visible, reinterpret_cast<const void *>(data),
            size, reinterpret_cast<const float *>(pose), reinterpret_cast<const float *>(normal), embedded);
    });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_deleteInstance(JNIEnv *env, jclass, jlong engine, jlong id) {
    jni::invokeVoid(env, "Delete Flywheel instance", [&] { instancing()->engine(static_cast<uint64_t>(engine))->deleteInstance(id); });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_updateInstanceLighting(
    JNIEnv *env, jclass, jlong engine, jlong id, jint scene, jfloat skyScale, jlong matrix) {
    jni::invokeVoid(env, "Update Flywheel instance lighting", [&] {
        instancing()->engine(static_cast<uint64_t>(engine))->updateInstanceLighting(id, scene, skyScale,
            reinterpret_cast<const float *>(matrix));
    });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_beginFrame(
    JNIEnv *env, jclass, jlong engine, jint x, jint y, jint z, jdouble ticks) {
    jni::invokeVoid(env, "Begin Flywheel frame", [&] { instancing()->engine(static_cast<uint64_t>(engine))->beginFrame({x, y, z}, ticks); });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_setShaderLights(
    JNIEnv *env, jclass, jlong engine, jlong directions, jint lightTextureId,
    jboolean constantAmbientLight) {
    jni::invokeVoid(env, "Set Flywheel shader light directions", [&] {
        instancing()->engine(static_cast<uint64_t>(engine))->setShaderLights(
            reinterpret_cast<const float *>(directions), lightTextureId,
            constantAmbientLight == JNI_TRUE);
    });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_render(JNIEnv *env, jclass, jlong engine) {
    jni::invokeVoid(env, "Prepare Flywheel instances", [&] { instancing()->engine(static_cast<uint64_t>(engine))->render(); });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_renderCrumbling(
    JNIEnv *env, jclass, jlong engine, jlong instances, jlong positions, jlong progress, jlong textures, jint count) {
    jni::invokeVoid(env, "Prepare Flywheel crumbling", [&] {
        instancing()->engine(static_cast<uint64_t>(engine))->renderCrumbling(reinterpret_cast<const uint64_t *>(instances),
            reinterpret_cast<const int64_t *>(positions), reinterpret_cast<const int32_t *>(progress),
            reinterpret_cast<const int32_t *>(textures), count);
    });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_NativeInstancingProxy_deleteEngine(JNIEnv *env, jclass, jlong engine) {
    jni::invokeVoid(env, "Delete Flywheel engine", [&] { instancing()->deleteEngine(static_cast<uint64_t>(engine)); });
}
