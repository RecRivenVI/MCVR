#pragma once

#include "core/all_extern.hpp"
#include "core/diagnostics/alloc_trace.hpp"
#include "core/failure_state.hpp"

#include <jni.h>

#include <exception>
#include <string>
#include <utility>

namespace jni {
enum class BoundaryPolicy { normal, initialization, allowAfterFatal };

inline void throwRuntimeException(JNIEnv *env, const char *operation, const char *message) noexcept {
    if (env == nullptr || env->ExceptionCheck()) { return; }
    try {
        const std::string text = std::string(operation) + ": " + message;
        if (jclass exceptionClass = env->FindClass("java/lang/IllegalStateException"); exceptionClass != nullptr) {
            env->ThrowNew(exceptionClass, text.c_str());
            env->DeleteLocalRef(exceptionClass);
        }
    } catch (...) {
        if (jclass exceptionClass = env->FindClass("java/lang/IllegalStateException"); exceptionClass != nullptr) {
            env->ThrowNew(exceptionClass, message);
            env->DeleteLocalRef(exceptionClass);
        }
    }
}

inline void recordFailure(const std::exception &exception, BoundaryPolicy policy) noexcept {
    if (policy == BoundaryPolicy::initialization) return;
    if (const auto *fatal = dynamic_cast<const mcvr::failure::FatalError *>(&exception)) {
        mcvr::failure::record(fatal->kind(), fatal->result(), fatal->operation(), fatal->what());
    }
}

inline void preflight(BoundaryPolicy policy) {
    if (policy == BoundaryPolicy::normal) mcvr::failure::throwIfFatal();
}

inline void recordUnknownFailure(BoundaryPolicy policy, const char *operation) noexcept {
    if (policy != BoundaryPolicy::initialization) {
        mcvr::failure::record(mcvr::failure::Kind::invariant, VK_ERROR_UNKNOWN,
                              operation, "unknown native exception crossed JNI boundary");
    }
}

template <typename Function>
jint invokeForVkResult(JNIEnv *env, const char *operation, Function &&function,
                       BoundaryPolicy policy = BoundaryPolicy::normal) noexcept {
    try {
        mcvr::diag::AllocTraceTagScope tagScope(operation);
        preflight(policy);
        return static_cast<jint>(std::forward<Function>(function)());
    } catch (const std::exception &exception) {
        recordFailure(exception, policy);
        throwRuntimeException(env, operation, exception.what());
    } catch (...) {
        recordUnknownFailure(policy, operation);
        throwRuntimeException(env, operation, "unknown native exception");
    }
    return static_cast<jint>(VK_ERROR_UNKNOWN);
}

template <typename Function>
void invokeVoid(JNIEnv *env, const char *operation, Function &&function,
                BoundaryPolicy policy = BoundaryPolicy::normal) noexcept {
    try {
        mcvr::diag::AllocTraceTagScope tagScope(operation);
        preflight(policy);
        std::forward<Function>(function)();
    } catch (const std::exception &exception) {
        recordFailure(exception, policy);
        throwRuntimeException(env, operation, exception.what());
    } catch (...) {
        recordUnknownFailure(policy, operation);
        throwRuntimeException(env, operation, "unknown native exception");
    }
}

template <typename Result, typename Function>
Result invoke(JNIEnv *env, const char *operation, Result fallback, Function &&function,
              BoundaryPolicy policy = BoundaryPolicy::normal) noexcept {
    try {
        mcvr::diag::AllocTraceTagScope tagScope(operation);
        preflight(policy);
        return static_cast<Result>(std::forward<Function>(function)());
    } catch (const std::exception &exception) {
        recordFailure(exception, policy);
        throwRuntimeException(env, operation, exception.what());
    } catch (...) {
        recordUnknownFailure(policy, operation);
        throwRuntimeException(env, operation, "unknown native exception");
    }
    return fallback;
}
} // namespace jni
