#include "com_radiance_client_proxy_vulkan_PipelineStateProxy_ViewportState.h"

#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/middleware/jni_exception.hpp"

extern "C" {
JNIEXPORT jintArray JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ViewportState_getViewport(
    JNIEnv *env, jclass) {
    jintArray result = nullptr;
    jni::invokeVoid(env, "Read viewport", [&] {
        auto framework = Renderer::instance().framework();
        auto frame = framework ? framework->safeAcquireCurrentContext() : nullptr;
        if (!frame) throw std::runtime_error("Viewport requires an acquired frame");
        auto ui = framework->pipeline()->acquirePipelineContext(frame)->uiModuleContext;
        const auto &viewport = ui->overlayViewportGl;
        jint values[]{viewport[0], viewport[1], viewport[2], viewport[3]};
        result = env->NewIntArray(4);
        if (result) env->SetIntArrayRegion(result, 0, 4, values);
    });
    return result;
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ViewportState_setScissorEnabled(
    JNIEnv *env, jclass, jboolean enabled) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ViewportState.setScissorEnabled", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayScissorEnabled(enabled);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ViewportState_setScissor(
    JNIEnv *env, jclass, jint x, jint y, jint width, jint height) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ViewportState.setScissor", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayScissor(x, y, width, height);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ViewportState_setViewport(
    JNIEnv *env, jclass, jint x, jint y, jint width, jint height) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ViewportState.setViewport", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayViewport(x, y, width, height);
    });
}
}
