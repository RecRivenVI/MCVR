#include "com_radiance_client_proxy_vulkan_PipelineStateProxy_DiagramState.h"

#include "core/middleware/jni_exception.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"

namespace {
std::shared_ptr<UIModuleContext> acquireUIContext() {
    auto framework = Renderer::instance().framework();
    if (framework == nullptr || !framework->isRunning()) return nullptr;
    auto context = framework->safeAcquireCurrentContext();
    if (context == nullptr) return nullptr;
    auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
    return pipelineContext == nullptr ? nullptr : pipelineContext->uiModuleContext;
}
}

extern "C" {
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DiagramState_beginTarget(
    JNIEnv *env, jclass, jint framebuffer, jint width, jint height) {
    jni::invokeVoid(env, "Begin diagram framebuffer", [&] {
        auto context = acquireUIContext();
        if (!context) throw std::runtime_error("Diagram requires an acquired frame");
        context->beginDiagramTarget(framebuffer, width, height);
    });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DiagramState_postTarget(
    JNIEnv *env, jclass, jint framebuffer) {
    jni::invokeVoid(env, "Post-process diagram framebuffer", [&] {
        auto context = acquireUIContext();
        if (!context) throw std::runtime_error("Diagram requires an acquired frame");
        context->postDiagramTarget(framebuffer);
    });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DiagramState_abortTarget(
    JNIEnv *env, jclass) {
    jni::invokeVoid(env, "Restore diagram framebuffer", [&] {
        auto context = acquireUIContext();
        if (!context) throw std::runtime_error("Diagram requires an acquired frame");
        context->abortDiagramTarget();
    });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DiagramState_begin(
    JNIEnv *env, jclass, jint x, jint y, jint width, jint height) {
    jni::invokeVoid(env, "Begin diagram render target", [&] {
        auto uiContext = acquireUIContext();
        if (uiContext != nullptr) uiContext->beginDiagram(x, y, width, height);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_PipelineStateProxy_00024DiagramState_post(
    JNIEnv *env, jclass) {
    jni::invokeVoid(env, "Composite diagram render target", [&] {
        auto uiContext = acquireUIContext();
        if (uiContext != nullptr) uiContext->postDiagram();
    });
}
}
