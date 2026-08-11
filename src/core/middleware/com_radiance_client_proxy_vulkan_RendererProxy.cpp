#include "core/render/streamline_runtime.hpp"
#include "com_radiance_client_proxy_vulkan_RendererProxy.h"

#include "core/all_extern.hpp"
#include "core/render/buffers.hpp"
#include "core/render/chunks.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/presentation_rates.hpp"
#include "core/render/textures.hpp"
#include "core/render/world.hpp"
#include "core/middleware/jni_exception.hpp"
#include "core/diagnostics/lifecycle_acceptance.hpp"
#include "core/diagnostics/audit_sink.hpp"
#include "core/diagnostics/device_loss_trace.hpp"
#include "core/middleware/jni_string.hpp"
#include "core/loading/loading_renderer.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#if defined(_WIN32)
#    include <windows.h>
using DYNLIB_HANDLE = HMODULE;

static DYNLIB_HANDLE try_get_loaded_handle(const wchar_t *wname) {
    return GetModuleHandleW(wname);
}

static FARPROC getproc(DYNLIB_HANDLE h, const char *sym) {
    FARPROC p = GetProcAddress(h, sym);
    if (!p) {
        throw std::runtime_error(std::string("GetProcAddress failed: ") + sym);
    }
    return p;
}

#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#    include <dlfcn.h>
using DYNLIB_HANDLE = void *;

static DYNLIB_HANDLE try_get_loaded_handle(const char *name) {
    return dlopen(name, RTLD_NOW | RTLD_NOLOAD);
}

static void *getproc(DYNLIB_HANDLE h, const char *sym) {
    void *p = dlsym(h, sym);
    if (!p) {
        throw std::runtime_error(std::string("dlsym failed: ") + sym + " - " + dlerror());
    }
    return p;
}

#else
#    error "Unsupported platform"
#endif

static DYNLIB_HANDLE bind_handle_from_candidates(JNIEnv *env, jobjectArray jnames) {
    if (jnames == nullptr) throw std::invalid_argument("GLFW candidate array is null");
    if (env->ExceptionCheck()) return nullptr;
    const jsize n = env->GetArrayLength(jnames);
    if (env->ExceptionCheck()) return nullptr;
    if (n == 0) return nullptr;
#if defined(_WIN32)
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    for (jsize i = 0; i < n; ++i) {
        jni::LocalRef s(env, static_cast<jstring>(env->GetObjectArrayElement(jnames, i)));
        if (env->ExceptionCheck()) return nullptr;
        if (!s) continue;
        const auto name = jni::copyUtf16(env, s.get());
        if (!name) {
            if (env->ExceptionCheck()) return nullptr;
            throw std::runtime_error("Cannot read GLFW module candidate");
        }
        const std::wstring terminated(name->begin(), name->end());
        DYNLIB_HANDLE h = try_get_loaded_handle(terminated.c_str());
        if (h) return h;
    }
#else
    for (jsize i = 0; i < n; ++i) {
        jni::LocalRef s(env, static_cast<jstring>(env->GetObjectArrayElement(jnames, i)));
        if (env->ExceptionCheck()) return nullptr;
        if (!s) continue;
        const auto name = jni::copyUtf8(env, s.get());
        if (!name) {
            if (env->ExceptionCheck()) return nullptr;
            throw std::runtime_error("Cannot read GLFW module candidate");
        }
        DYNLIB_HANDLE h = try_get_loaded_handle(name->c_str());
        if (h) return h;
    }
#endif
    return nullptr;
}

static void bind_symbols(DYNLIB_HANDLE h) {
#if defined(_WIN32)
    auto gp = [&](const char *sym) { return getproc(h, sym); };
#else
    auto gp = [&](const char *sym) { return getproc(h, sym); };
#endif
#ifdef _WIN32
    p_glfwGetWin32Window = reinterpret_cast<HWND (*)(GLFWwindow *)>(gp("glfwGetWin32Window"));
#endif
    p_glfwInit = reinterpret_cast<PFN_glfwInit>(gp("glfwInit"));
    p_glfwTerminate = reinterpret_cast<PFN_glfwTerminate>(gp("glfwTerminate"));
    p_glfwGetWindowSize = reinterpret_cast<PFN_glfwGetWindowSize>(gp("glfwGetWindowSize"));
    p_glfwGetWindowAttrib = reinterpret_cast<PFN_glfwGetWindowAttrib>(gp("glfwGetWindowAttrib"));
    p_glfwCreateWindowSurface = reinterpret_cast<PFN_glfwCreateWindowSurface>(gp("glfwCreateWindowSurface"));
    p_glfwGetRequiredInstanceExtensions =
        reinterpret_cast<PFN_glfwGetRequiredInstanceExtensions>(gp("glfwGetRequiredInstanceExtensions"));
    p_glfwSetWindowTitle = reinterpret_cast<PFN_glfwSetWindowTitle>(gp("glfwSetWindowTitle"));
    p_glfwSetFramebufferSizeCallback =
        reinterpret_cast<PFN_glfwSetFramebufferSizeCallback>(gp("glfwSetFramebufferSizeCallback"));
    p_glfwGetFramebufferSize = reinterpret_cast<PFN_glfwGetFramebufferSize>(gp("glfwGetFramebufferSize"));
    p_glfwWaitEvents = reinterpret_cast<PFN_glfwWaitEvents>(gp("glfwWaitEvents"));
    p_glfwWaitEventsTimeout = reinterpret_cast<PFN_glfwWaitEventsTimeout>(gp("glfwWaitEventsTimeout"));
    p_glfwWindowShouldClose = reinterpret_cast<PFN_glfwWindowShouldClose>(gp("glfwWindowShouldClose"));
}

bool bindLoadedGlfw(JNIEnv *env, jobjectArray candidates) {
    auto handle = bind_handle_from_candidates(env, candidates);
    if (env->ExceptionCheck()) return false;
    if (!handle) throw std::runtime_error("Could not bind the already-loaded GLFW library");
    bind_symbols(handle);
    return true;
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_initFolderPath(JNIEnv *env,
                                                                                          jclass,
                                                                                          jstring folderPath) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.RendererProxy.initFolderPath", [&] {
            if (folderPath == NULL) { return; }

            const auto path = jni::copyUtf16(env, folderPath);
            if (!path) {
                if (env->ExceptionCheck()) return;
                throw std::runtime_error("Cannot read renderer folder path");
            }
            Renderer::folderPath = *path;
    });
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_initRendererNative(
    JNIEnv *env, jclass, jobjectArray candidates, jlong windowHandle) {
    if (!Renderer::is_initialized()) mcvr::failure::clearForInitialization();
    return jni::invokeForVkResult(env, "Renderer initialization", [&] {
        GLFWwindow *window = (GLFWwindow *)(intptr_t)windowHandle;
        if (Renderer::is_initialized()) {
            auto framework = Renderer::instance().framework();
            if (!framework || framework->window()->window() != window)
                throw std::runtime_error("Early renderer window differs from the game window");
            return framework->safeAcquireCurrentContext() ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!bindLoadedGlfw(env, candidates)) return VK_ERROR_INITIALIZATION_FAILED;
        Renderer::init(window);
        mcvr::presentationRates.reset();
        return Renderer::instance().framework()->acquireContext();
    }, jni::BoundaryPolicy::initialization);
}

JNIEXPORT jlong JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_presentationRatesNative(JNIEnv *env, jclass) {
    return jni::invoke<jlong>(env, "JNI com.radiance.client.proxy.vulkan.RendererProxy.presentationRatesNative", 0, [&]() -> jlong {
            return static_cast<jlong>(mcvr::presentationRates.snapshot());
    });
}

JNIEXPORT jlong JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_sectionCountsNative(JNIEnv *env, jclass) {
    return jni::invoke<jlong>(env, "Read PT section counts", -1, [&]() -> jlong {
        auto world = Renderer::instance().world();
        auto chunks = world ? world->chunks() : nullptr;
        if (!chunks) return -1;
        std::lock_guard<std::recursive_mutex> lock(chunks->mutex());
        const auto &sections = chunks->chunks();
        uint32_t ready = 0;
        // Same eligibility condition as the chunk loop in WorldPrepareContext.
        // PT includes off-screen geometry for secondary rays, unlike vanilla C.
        for (const auto &section : sections) {
            if (section && section->blas) ++ready;
        }
        return static_cast<jlong>((uint64_t(ready) << 32) | uint32_t(sections.size()));
    });
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_maxSupportedTextureSize(JNIEnv *env, jclass) {
    return jni::invoke<jint>(env, "JNI com.radiance.client.proxy.vulkan.RendererProxy.maxSupportedTextureSize", 0, [&]() -> jint {
            auto maxImageSize = Renderer::instance().framework()->physicalDevice()->properties().limits.maxImageDimension2D;
            return maxImageSize;
    });
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_capabilityLimit(JNIEnv *env, jclass, jint name) {
    jint value = 0;
    jni::invokeVoid(env, "Query Vulkan compatibility limit", [&] {
        auto framework = Renderer::instance().framework();
        if (!framework || !framework->physicalDevice()) throw std::runtime_error("Vulkan device is unavailable");
        const auto &limits = framework->physicalDevice()->properties().limits;
        switch (name) {
        case 0x8B4D: value = std::min(4096u, limits.maxPerStageDescriptorSampledImages); break;
        case 0x8CDF: value = std::min(32u, limits.maxColorAttachments); break;
        case 0x8A2F: value = limits.maxDescriptorSetUniformBuffers; break;
        case 0x8A30: value = limits.maxUniformBufferRange; break;
        case 0x8A34: value = static_cast<jint>(limits.minUniformBufferOffsetAlignment); break;
        case 0x8869: value = limits.maxVertexInputAttributes; break;
        case 0x82D9: value = limits.maxVertexInputAttributeOffset; break;
        case 0x9315: value = limits.maxFramebufferWidth; break;
        case 0x9316: value = limits.maxFramebufferHeight; break;
        case 0x9122: value = limits.maxVertexOutputComponents; break;
        case 0x9125: value = limits.maxFragmentInputComponents; break;
        case 0x0D33: value = limits.maxImageDimension2D; break;
        default: throw std::invalid_argument("Unmapped compatibility limit: " + std::to_string(name));
        }
    });
    return value;
}

JNIEXPORT jboolean JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_tessellationSupported(JNIEnv *env, jclass) {
    return jni::invoke<jboolean>(env, "JNI com.radiance.client.proxy.vulkan.RendererProxy.tessellationSupported", JNI_FALSE, [&]() -> jboolean {
            auto framework = Renderer::instance().framework();
            if (!framework || !framework->device() || !framework->pipeline()
                || !framework->pipeline()->uiModule()) return JNI_FALSE;
            return framework->device()->hasTessellation() ? JNI_TRUE : JNI_FALSE;
    });
}

JNIEXPORT jstring JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_backendString(JNIEnv *env, jclass, jint name) {
    return jni::invoke<jstring>(env, "Read Vulkan backend string", nullptr, [&]() -> jstring {
        const VkPhysicalDeviceProperties properties =
            Renderer::instance().framework()->physicalDevice()->properties();
        std::string value;
        switch (name) {
            case 7936: // GL_VENDOR, retained as the Blaze3D query contract.
                switch (properties.vendorID) {
                    case 0x1002:
                        value = "AMD";
                        break;
                    case 0x1010:
                        value = "Imagination Technologies";
                        break;
                    case 0x106B:
                        value = "Apple";
                        break;
                    case 0x10DE:
                        value = "NVIDIA";
                        break;
                    case 0x13B5:
                        value = "Arm";
                        break;
                    case 0x5143:
                        value = "Qualcomm";
                        break;
                    case 0x8086:
                        value = "Intel";
                        break;
                    default: {
                        std::ostringstream stream;
                        stream << "Vulkan vendor 0x" << std::hex << std::uppercase << properties.vendorID;
                        value = stream.str();
                        break;
                    }
                }
                break;
            case 7937: // GL_RENDERER
                value = properties.deviceName;
                break;
            case 7938: // GL_VERSION
                value = "Vulkan " + std::to_string(VK_VERSION_MAJOR(properties.apiVersion)) + "." +
                        std::to_string(VK_VERSION_MINOR(properties.apiVersion)) + "." +
                        std::to_string(VK_VERSION_PATCH(properties.apiVersion));
                break;
            default:
                value = "Unsupported Vulkan diagnostic query " + std::to_string(name);
                break;
        }
        return env->NewStringUTF(value.c_str());
    });
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_acquireContextNative(JNIEnv *env, jclass) {
    return jni::invokeForVkResult(env, "Acquire frame", [] {
        auto framework = Renderer::instance().framework();
        return framework == nullptr ? VK_NOT_READY : framework->acquireContext();
    });
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_submitCommandNative(JNIEnv *env, jclass) {
    if (mcvr::audit::enabled(MCVR_AUDIT_LIFECYCLE)) mcvr::diagnostics::lifecycle::noteSubmitAttempt();
    return jni::invokeForVkResult(env, "Submit frame", [] {
        auto framework = Renderer::instance().framework();
        const auto result = framework == nullptr ? VK_NOT_READY : framework->submitCommand();
        if (result == VK_SUCCESS && mcvr::audit::enabled(MCVR_AUDIT_LIFECYCLE)) mcvr::diagnostics::lifecycle::noteSuccessfulSubmit();
        mcvr::StreamlineRuntime::get().marker(sl::PCLMarker::eRenderSubmitEnd);
        return result;
    });
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_presentNative(JNIEnv *env, jclass) {
    return jni::invokeForVkResult(env, "Present frame", [] {
        auto framework = Renderer::instance().framework();
        return framework == nullptr ? VK_NOT_READY : framework->present();
    });
}

JNIEXPORT jint JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_warmupCurrentPipelineNative(JNIEnv *env, jclass) {
    return jni::invokeForVkResult(env, "Warm up current world pipeline", [] {
        auto framework = Renderer::instance().framework();
        return framework == nullptr ? VK_NOT_READY : framework->warmupCurrentPipeline();
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_fuseWorld(JNIEnv *env, jclass) {
    jni::invokeVoid(env, "Fuse world", [] {
        auto framework = Renderer::instance().framework();
        if (framework == nullptr || !framework->isRunning()) return;
        auto context = framework->safeAcquireCurrentContext();
        if (context == nullptr) return;
        auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
        if (pipelineContext != nullptr) { pipelineContext->fuseWorld(); }
    });
}

namespace {
template <typename Writer>
bool updateCurrentCameraEffects(Writer &&writer) {
    auto framework = Renderer::instance().framework();
    if (framework == nullptr || !framework->isRunning() || framework->safeAcquireCurrentContext() == nullptr) {
        return false;
    }
    auto buffers = Renderer::instance().buffers();
    auto buffer = buffers == nullptr ? nullptr : buffers->worldUniformBuffer();
    if (buffer == nullptr || buffer->mappedPtr() == nullptr || buffer->size() < sizeof(vk::Data::WorldUBO)) {
        return false;
    }
    auto *ubo = static_cast<vk::Data::WorldUBO *>(buffer->mappedPtr());
    writer(*ubo);
    buffer->flush();
    return true;
}

bool validCameraEffectTexture(jint textureId) {
    return textureId >= 0 && textureId < 4096;
}
} // namespace

JNIEXPORT jboolean JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_cameraBlockEffect(
    JNIEnv *env, jclass, jint textureId, jfloat u0, jfloat v0, jfloat u1, jfloat v1,
    jfloat lightScale) {
    jboolean recorded = JNI_FALSE;
    jni::invokeVoid(env, "Record HDR block camera effect", [&] {
        if (!validCameraEffectTexture(textureId)) return;
        recorded = updateCurrentCameraEffects([&](vk::Data::WorldUBO &ubo) {
            ubo.cameraEffectTextureIDs.x = textureId;
            ubo.cameraBlockUV = glm::vec4(u0, v0, u1, v1);
            const float light = std::clamp(lightScale, 0.0f, 1.0f);
            ubo.cameraBlockColor = glm::vec4(light, light, light, 1.0f);
        }) ? JNI_TRUE : JNI_FALSE;
    });
    return recorded;
}

JNIEXPORT jboolean JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_cameraFluidEffect(
    JNIEnv *env, jclass, jint textureId, jfloat uBase, jfloat vBase,
    jfloat repeatU, jfloat repeatV, jfloat alpha, jfloat brightness) {
    jboolean recorded = JNI_FALSE;
    jni::invokeVoid(env, "Record HDR fluid camera effect", [&] {
        if (!validCameraEffectTexture(textureId)) return;
        recorded = updateCurrentCameraEffects([&](vk::Data::WorldUBO &ubo) {
            ubo.cameraEffectTextureIDs.y = textureId;
            ubo.cameraFluidParams = glm::vec4(uBase, vBase, repeatU,
                                               std::clamp(alpha, 0.0f, 1.0f));
            const float light = std::max(brightness, 0.0f);
            ubo.cameraFluidColor = glm::vec4(light, light, light, repeatV);
        }) ? JNI_TRUE : JNI_FALSE;
    });
    return recorded;
}

JNIEXPORT jboolean JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_cameraFireEffect(
    JNIEnv *env, jclass, jint textureId, jfloat u0, jfloat v0, jfloat u1, jfloat v1,
    jfloat alpha, jfloat emissionScale) {
    jboolean recorded = JNI_FALSE;
    jni::invokeVoid(env, "Record HDR fire camera effect", [&] {
        if (!validCameraEffectTexture(textureId)) return;
        recorded = updateCurrentCameraEffects([&](vk::Data::WorldUBO &ubo) {
            ubo.cameraEffectTextureIDs.z = textureId;
            ubo.cameraFireUV = glm::vec4(u0, v0, u1, v1);
            ubo.cameraFireParams = glm::vec4(std::clamp(alpha, 0.0f, 1.0f),
                                             std::max(emissionScale, 0.0f), 0.0f, 0.0f);
        }) ? JNI_TRUE : JNI_FALSE;
    });
    return recorded;
}

JNIEXPORT jboolean JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_postBlur(JNIEnv *env, jclass) {
    jboolean recorded = JNI_FALSE;
    jni::invokeVoid(env, "Post blur", [&] {
        auto framework = Renderer::instance().framework();
        if (framework == nullptr || !framework->isRunning()) return;
        auto context = framework->safeAcquireCurrentContext();
        if (context == nullptr) return;
        auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
        if (pipelineContext != nullptr && pipelineContext->uiModuleContext != nullptr) {
            pipelineContext->uiModuleContext->postBlur(6);
            recorded = JNI_TRUE;
        }
    });
    return recorded;
}

JNIEXPORT jboolean JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_postEntityEffect(JNIEnv *env, jclass, jint effect) {
    jboolean recorded = JNI_FALSE;
    jni::invokeVoid(env, "Vanilla entity post effect", [effect, &recorded] {
        OverlayPostPipelineType type;
        switch (effect) {
            case 0:
                type = CREEPER;
                break;
            case 1:
                type = SPIDER;
                break;
            case 2:
                type = INVERT;
                break;
            default:
                throw std::invalid_argument("Unknown vanilla entity post-effect id");
        }

        auto framework = Renderer::instance().framework();
        if (framework == nullptr || !framework->isRunning()) return;
        auto context = framework->safeAcquireCurrentContext();
        if (context == nullptr) return;
        auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
        if (pipelineContext != nullptr && pipelineContext->uiModuleContext != nullptr) {
            pipelineContext->uiModuleContext->postEntityEffect(type);
            recorded = JNI_TRUE;
        }
    });
    return recorded;
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_beginGpuProfile(JNIEnv *env, jclass) {
    return jni::invoke<jint>(env, "JNI com.radiance.client.proxy.vulkan.RendererProxy.beginGpuProfile", 0, [&]() -> jint {
            auto framework = Renderer::instance().framework();
            return framework == nullptr ? 1 : static_cast<jint>(framework->beginGpuProfile());
    });
}

JNIEXPORT jboolean JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_isGpuProfileReady(JNIEnv *env, jclass, jint sequence) {
    return jni::invoke<jboolean>(env, "JNI com.radiance.client.proxy.vulkan.RendererProxy.isGpuProfileReady", JNI_FALSE, [&]() -> jboolean {
            auto framework = Renderer::instance().framework();
            return framework == nullptr || framework->isGpuProfileReady(static_cast<uint32_t>(sequence)) ? JNI_TRUE : JNI_FALSE;
    });
}

JNIEXPORT jlong JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_gpuProfileTimeNs(JNIEnv *env, jclass, jint sequence) {
    return jni::invoke<jlong>(env, "JNI com.radiance.client.proxy.vulkan.RendererProxy.gpuProfileTimeNs", 0, [&]() -> jlong {
            auto framework = Renderer::instance().framework();
            return framework == nullptr ? 0 : static_cast<jlong>(framework->gpuProfileTimeNs(static_cast<uint32_t>(sequence)));
    });
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_closeNative(JNIEnv *env, jclass) {
    if (mcvr::audit::enabled(MCVR_AUDIT_LIFECYCLE)) mcvr::diagnostics::lifecycle::noteClose();
    return jni::invokeForVkResult(env, "Close renderer", [] {
        if (!Renderer::is_initialized()) { return VK_SUCCESS; }
        auto framework = Renderer::instance().framework();
        if (framework == nullptr) { return VK_SUCCESS; }
        const VkResult priorFailure = framework->lastFailure();
        Renderer::instance().close();
        return priorFailure;
    }, jni::BoundaryPolicy::allowAfterFatal);
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_injectRuntimeFatalForAcceptanceNative(
    JNIEnv *env, jclass) {
    if (!mcvr::audit::enabled(MCVR_AUDIT_LIFECYCLE)) {
        if (!env->ExceptionCheck()) {
            auto type = env->FindClass("java/lang/IllegalStateException");
            if (type) { env->ThrowNew(type, "Lifecycle experiment is not armed by Radiance Audit"); env->DeleteLocalRef(type); }
        }
        return;
    }
    jni::invokeVoid(env, "Lifecycle acceptance G1 runtime fatal", [] {
        mcvr::diagnostics::lifecycle::noteInjection();
        mcvr::failure::raise(mcvr::failure::Kind::runtime, VK_ERROR_UNKNOWN,
                             "lifecycle-acceptance/G1-runtime-fatal");
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_probeNormalCallAfterFatalNative(
    JNIEnv *env, jclass) {
    jni::invokeVoid(env, "Lifecycle acceptance normal-call probe", [] {
        mcvr::diagnostics::lifecycle::noteRejectedProbeBody();
    });
}

JNIEXPORT jstring JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_lifecycleAcceptanceStateNative(
    JNIEnv *env, jclass) {
    return jni::invoke<jstring>(env, "Read lifecycle acceptance state", nullptr, [&] {
        const auto nativeFailure = mcvr::failure::snapshot();
        const std::string state = mcvr::diagnostics::lifecycle::describe()
            + " fatal=" + (nativeFailure.fatal ? "true" : "false")
            + " deviceLost=" + (mcvr::failure::isDeviceLost() ? "true" : "false")
            + " firstResult=" + std::to_string(nativeFailure.result)
            + " firstOperation=" + nativeFailure.operation;
        return env->NewStringUTF(state.c_str());
    }, jni::BoundaryPolicy::allowAfterFatal);
}

JNIEXPORT jstring JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_roundTripAcceptanceStringNative(
    JNIEnv *env, jclass, jstring input) {
    return jni::invoke<jstring>(env, "Lifecycle acceptance JNI string round trip", nullptr, [&] {
        const auto value = jni::copyUtf16(env, input);
        if (!value.has_value()) return static_cast<jstring>(nullptr);
        return env->NewString(reinterpret_cast<const jchar *>(value->data()),
                              static_cast<jsize>(value->size()));
    }, jni::BoundaryPolicy::allowAfterFatal);
}

JNIEXPORT jint JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_beginResourceReloadNative(JNIEnv *env, jclass) {
    mcvr::diagnostics::device_loss::note("resource-reload-begin");
    return jni::invokeForVkResult(env, "Begin resource reload", [] {
        auto textures = Renderer::instance().textures();
        return textures == nullptr ? VK_NOT_READY : textures->beginResourceReload();
    });
}

JNIEXPORT jint JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_endResourceReloadNative(JNIEnv *env, jclass) {
    mcvr::diagnostics::device_loss::note("resource-reload-end");
    return jni::invokeForVkResult(env, "End resource reload", [] {
        auto textures = Renderer::instance().textures();
        return textures == nullptr ? VK_NOT_READY : textures->endResourceReload();
    });
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_shouldRenderWorld(JNIEnv *env, jclass, jboolean shouldRenderWorld) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.RendererProxy.shouldRenderWorld", [&] {
            auto world = Renderer::instance().world();
            if (world == nullptr) return;
            world->shouldRender() = shouldRenderWorld;
    });
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_takeScreenshotNative(
    JNIEnv *env, jclass, jboolean withUI, jint width, jint height, jint channel, jlong pointer) {
    return jni::invokeForVkResult(env, "Take screenshot", [&] {
        auto framework = Renderer::instance().framework();
        return framework == nullptr ? VK_NOT_READY :
                                      framework->takeScreenshot(withUI, width, height, channel,
                                                                reinterpret_cast<void *>(pointer));
    });
}

JNIEXPORT jstring JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_lastFailureDescriptionNative(
    JNIEnv *env, jclass) {
    return jni::invoke<jstring>(env, "Read renderer failure", nullptr, [&]() -> jstring {
        const auto nativeFailure = mcvr::failure::snapshot();
        if (nativeFailure.fatal) return env->NewStringUTF(nativeFailure.description.c_str());
        if (!Renderer::is_initialized()) return env->NewStringUTF("");
        auto framework = Renderer::instance().framework();
        const std::string description = framework == nullptr ? std::string{} : framework->lastFailureDescription();
        return env->NewStringUTF(description.c_str());
    }, jni::BoundaryPolicy::allowAfterFatal);
}

extern "C" JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_streamlineFrameEvent(JNIEnv *env,jclass,jint event) {
    jni::invokeVoid(env, "JNI com.radiance.client.proxy.vulkan.RendererProxy.streamlineFrameEvent", [&] {
            if (!Renderer::is_initialized()) return;
            auto &sl=mcvr::StreamlineRuntime::get();
            switch(event) {
            case 0: sl.beginFrame(Renderer::options.dlssFrameGeneration ? std::max(1,Renderer::options.reflexMode) : Renderer::options.reflexMode, Renderer::instance().framework()->effectiveFrameRateLimit()); break;
            case 1: sl.marker(sl::PCLMarker::eSimulationStart); break;
            case 2: sl.marker(sl::PCLMarker::eSimulationEnd); sl.marker(sl::PCLMarker::eRenderSubmitStart); break;
            case 3: sl.marker(sl::PCLMarker::eRenderSubmitEnd); break;
            }
    });
}
