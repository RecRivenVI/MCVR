#include "com_radiance_client_proxy_vulkan_PipelineStateProxy_DepthStencilState.h"
#include "core/middleware/jni_exception.hpp"

#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"

extern "C" {
JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DepthStencilState_setDepthTestEnable(JNIEnv *env,
                                                                                                   jclass,
                                                                                                   jboolean enable) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$DepthStencilState.setDepthTestEnable", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayDepthTestEnable(enable);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DepthStencilState_setDepthWriteEnable(JNIEnv *env,
                                                                                                    jclass,
                                                                                                    jboolean enable) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$DepthStencilState.setDepthWriteEnable", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayDepthWriteEnable(enable);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DepthStencilState_setStencilTestEnable(JNIEnv *env,
                                                                                                     jclass,
                                                                                                     jboolean enable) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$DepthStencilState.setStencilTestEnable", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayStencilTestEnable(enable);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DepthStencilState_vkSetDepthCompareOp(
    JNIEnv *env, jclass, jint depthCompareOp) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$DepthStencilState.vkSetDepthCompareOp", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayDepthCompareOp(depthCompareOp);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DepthStencilState_vkSetStencilFrontFunc(
    JNIEnv *env, jclass, jint compareOp, jint reference, jint compareMask) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$DepthStencilState.vkSetStencilFrontFunc", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayStencilFrontFunc(compareOp, reference, compareMask);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DepthStencilState_vkSetStencilBackFunc(
    JNIEnv *env, jclass, jint compareOp, jint reference, jint compareMask) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$DepthStencilState.vkSetStencilBackFunc", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayStencilBackFunc(compareOp, reference, compareMask);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DepthStencilState_vkSetStencilFrontOp(
    JNIEnv *env, jclass, jint failOp, jint depthFailOp, jint passOp) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$DepthStencilState.vkSetStencilFrontOp", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayStencilFrontOp(failOp, depthFailOp, passOp);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DepthStencilState_vkSetStencilBackOp(
    JNIEnv *env, jclass, jint failOp, jint depthFailOp, jint passOp) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$DepthStencilState.vkSetStencilBackOp", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayStencilBackOp(failOp, depthFailOp, passOp);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DepthStencilState_vkSetStencilFrontWriteMask(
    JNIEnv *env, jclass, jint writeMask) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$DepthStencilState.vkSetStencilFrontWriteMask", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayStencilFrontWriteMask(writeMask);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DepthStencilState_vkSetStencilBackWriteMask(
    JNIEnv *env, jclass, jint writeMask) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$DepthStencilState.vkSetStencilBackWriteMask", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayStencilBackWriteMask(writeMask);
    });
}
}