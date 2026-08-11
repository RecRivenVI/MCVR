#include "com_radiance_client_proxy_vulkan_WindowProxy.h"
#include "core/middleware/jni_exception.hpp"

#include "core/all_extern.hpp"
#include "core/vulkan/window.hpp"

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_WindowProxy_onFramebufferSizeChanged(JNIEnv *env,
                                                                                                            jclass) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.WindowProxy.onFramebufferSizeChanged", [&] {
            vk::Window::framebufferResized = true;
    });
}