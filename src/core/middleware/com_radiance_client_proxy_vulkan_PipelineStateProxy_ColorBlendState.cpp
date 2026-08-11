#include "com_radiance_client_proxy_vulkan_PipelineStateProxy_ColorBlendState.h"
#include "core/middleware/jni_exception.hpp"

#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"

extern "C" {

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ColorBlendState_setBlendEnable(
    JNIEnv *env, jclass, jboolean enable) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ColorBlendState.setBlendEnable", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayBlendEnable(enable);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ColorBlendState_setColorBlendConstants(
    JNIEnv *env, jclass, jfloat const1, jfloat const2, jfloat const3, jfloat const4) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ColorBlendState.setColorBlendConstants", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayColorBlendConstants(const1, const2, const3, const4);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ColorBlendState_setColorLogicOpEnable(JNIEnv *env,
                                                                                                    jclass,
                                                                                                    jboolean enable) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ColorBlendState.setColorLogicOpEnable", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayColorLogicOpEnable(enable);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ColorBlendState_vkSetBlendFuncSeparate(
    JNIEnv *env,
    jclass,
    jint srcColorBlendFactor,
    jint srcAlphaBlendFactor,
    jint dstColorBlendFactor,
    jint dstAlphaBlendFactor) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ColorBlendState.vkSetBlendFuncSeparate", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayBlendFuncSeparate(srcColorBlendFactor, srcAlphaBlendFactor,
                                                                          dstColorBlendFactor, dstAlphaBlendFactor);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ColorBlendState_vkSetBlendOpSeparate(JNIEnv *env,
                                                                                                   jclass,
                                                                                                   jint colorBlendOp,
                                                                                                   jint alphaBlendOp) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ColorBlendState.vkSetBlendOpSeparate", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayBlendOpSeparate(colorBlendOp, alphaBlendOp);
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ColorBlendState_vkSetColorWriteMask(JNIEnv *env,
                                                                                                  jclass,
                                                                                                  jint colorWriteMask) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ColorBlendState.vkSetColorWriteMask", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayColorWriteMask(colorWriteMask);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024ColorBlendState_vkSetColorLogicOp(
    JNIEnv *env, jclass, jint colorLogicOp) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.PipelineStateProxy$ColorBlendState.vkSetColorLogicOp", [&] {
            auto framework = Renderer::instance().framework();
            if (framework == nullptr) return;
            auto context = framework->safeAcquireCurrentContext();
            auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
            pipelineContext->uiModuleContext->setOverlayColorLogicOp(colorLogicOp);
    });
}
}