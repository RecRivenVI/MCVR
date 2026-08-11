#include "com_radiance_client_pipeline_Pipeline.h"

#include "core/render/modules/world/ray_tracing/ray_tracing_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/middleware/jni_exception.hpp"
#include "core/middleware/jni_string.hpp"

#include <iostream>
#include <vector>

JNIEXPORT void JNICALL Java_com_radiance_client_pipeline_Pipeline_buildNative(JNIEnv *env,
                                                                              jclass,
                                                                              jlong paramsLongPtr) {
    jni::invokeVoid(env, "Build world pipeline", [&] {
        WorldPipelineBuildParams *params = reinterpret_cast<WorldPipelineBuildParams *>(paramsLongPtr);
        auto framework = Renderer::instance().framework();
        if (framework == nullptr || !framework->isRunning()) {
            throw std::runtime_error("renderer is not available");
        }
        auto pipeline = framework->pipeline();
        if (pipeline != nullptr) { pipeline->buildWorldPipelineBlueprint(params); }
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_pipeline_Pipeline_collectNativeModules(JNIEnv *env, jclass) {
    jni::invokeVoid(env, "Collect native render modules", [] { Pipeline::collectWorldModules(); });
}

JNIEXPORT jboolean JNICALL Java_com_radiance_client_pipeline_Pipeline_isNativeRebuildActive(JNIEnv *env, jclass) {
    return jni::invoke<jboolean>(env, "JNI com.radiance.client.pipeline.Pipeline.isNativeRebuildActive", JNI_FALSE, [&]() -> jboolean {
            return Pipeline::nativeRebuildActive() ? JNI_TRUE : JNI_FALSE;
    });
}

JNIEXPORT jboolean JNICALL Java_com_radiance_client_pipeline_Pipeline_isNativeModuleAvailable(JNIEnv *env,
                                                                                              jclass,
                                                                                              jstring name) {
    return jni::invoke<jboolean>(env, "JNI com.radiance.client.pipeline.Pipeline.isNativeModuleAvailable", JNI_FALSE, [&]() -> jboolean {
            if (name == nullptr) return JNI_FALSE;
            const auto nativeString = jni::copyUtf8(env, name);
            if (!nativeString) {
                if (env->ExceptionCheck()) throw std::runtime_error("Cannot read native module name");
                return JNI_FALSE;
            }
            bool available = Pipeline::worldModuleConstructors.find(*nativeString) != Pipeline::worldModuleConstructors.end();
            return available ? JNI_TRUE : JNI_FALSE;
    });
}

JNIEXPORT jstring JNICALL Java_com_radiance_client_pipeline_Pipeline_getAttributes(JNIEnv *env,
                                                                                   jclass,
                                                                                   jstring name,
                                                                                   jobjectArray attributes,
                                                                                   jstring languageObject) {
    return jni::invoke<jstring>(env, "JNI com.radiance.client.pipeline.Pipeline.getAttributes", nullptr, [&]() -> jstring {
            if (name == nullptr) {
                return env->NewStringUTF("{}");
            }

            const auto moduleName = jni::copyUtf8(env, name);
            if (!moduleName) throw std::runtime_error("Cannot read native module name");

            std::vector<std::string> attributeList;
            if (attributes != nullptr) {
                jsize count = env->GetArrayLength(attributes);
                attributeList.reserve(static_cast<size_t>(count));
                for (jsize i = 0; i < count; i++) {
                    jni::LocalRef entry(env, static_cast<jstring>(env->GetObjectArrayElement(attributes, i)));
                    if (env->ExceptionCheck()) throw std::runtime_error("Cannot read module attribute");
                    if (!entry) {
                        attributeList.emplace_back();
                        continue;
                    }
                    const auto entryText = jni::copyUtf8(env, entry.get());
                    if (!entryText) throw std::runtime_error("Cannot read module attribute");
                    attributeList.push_back(*entryText);
                }
            }

            std::string language = "en_us";
            if (languageObject != nullptr) {
                const auto languageText = jni::copyUtf8(env, languageObject);
                if (!languageText) throw std::runtime_error("Cannot read language code");
                language = *languageText;
            }

            std::string result = "{}";
            if (*moduleName == std::string(RayTracingModule::NAME)) {
                RayTracingModule rayTracingModule;
                result = rayTracingModule.getAttributes(attributeList, language);
            }

            return env->NewStringUTF(result.c_str());
    });
}
