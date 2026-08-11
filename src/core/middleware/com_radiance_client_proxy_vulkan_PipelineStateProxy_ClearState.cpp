#include "com_radiance_client_proxy_vulkan_PipelineStateProxy_ClearState.h"
#include "core/middleware/jni_exception.hpp"

#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/world.hpp"

extern "C" {
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ClearState_setClearColor(
    JNIEnv *env, jclass, jfloat red, jfloat green, jfloat blue, jfloat alpha) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ClearState.setClearColor", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayClearColor(red, green, blue, alpha);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ClearState_setClearDepth(
    JNIEnv *env, jclass, jdouble depth) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ClearState.setClearDepth", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayClearDepth(depth);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ClearState_setClearStencil(
    JNIEnv *env, jclass, jint stencil) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ClearState.setClearStencil", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayClearStencil(stencil);
    });
}
}
