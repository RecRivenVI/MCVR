#include "com_radiance_client_proxy_world_PlayerProxy.h"
#include "core/middleware/jni_exception.hpp"

#include "core/render/chunks.hpp"
#include "core/render/renderer.hpp"

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_world_PlayerProxy_setCameraPos(JNIEnv *env, jclass, jdouble x, jdouble y, jdouble z) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.world.PlayerProxy.setCameraPos", [&] {
            auto world = Renderer::instance().world();
            if (world == nullptr) return;
            Renderer::instance().world()->setCameraPos(glm::dvec3{x, y, z});
    });
}