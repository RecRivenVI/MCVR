#pragma once

#include "core/util/borrowed_string.hpp"

#include <jni.h>

#include <optional>
#include <string>
#include <type_traits>

namespace jni {

template <typename Reference>
class LocalRef final {
  public:
    LocalRef(JNIEnv *env, Reference value) noexcept : env_(env), value_(value) {}
    ~LocalRef() {
        if (env_ != nullptr && value_ != nullptr) env_->DeleteLocalRef(value_);
    }
    LocalRef(const LocalRef &) = delete;
    LocalRef &operator=(const LocalRef &) = delete;
    Reference get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

  private:
    JNIEnv *env_;
    Reference value_;
};

inline std::optional<std::u16string> copyUtf16(JNIEnv *env, jstring value) {
    if (env == nullptr) return std::nullopt;
    return mcvr::detail::copyBorrowedString<char16_t>(
        value,
        [&](jstring string) { return env->GetStringLength(string); },
        [&](jstring string) {
            return reinterpret_cast<const char16_t *>(env->GetStringChars(string, nullptr));
        },
        [&](jstring string, const char16_t *characters) {
            env->ReleaseStringChars(string, reinterpret_cast<const jchar *>(characters));
        },
        [&] { return env->ExceptionCheck() == JNI_TRUE; });
}

inline std::optional<std::string> copyUtf8(JNIEnv *env, jstring value) {
    if (env == nullptr) return std::nullopt;
    return mcvr::detail::copyBorrowedString<char>(
        value,
        [&](jstring string) { return env->GetStringUTFLength(string); },
        [&](jstring string) { return env->GetStringUTFChars(string, nullptr); },
        [&](jstring string, const char *characters) { env->ReleaseStringUTFChars(string, characters); },
        [&] { return env->ExceptionCheck() == JNI_TRUE; });
}

} // namespace jni
