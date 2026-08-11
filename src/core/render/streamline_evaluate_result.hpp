#pragma once
#include <sl_result.h>

namespace mcvr {
// Streamline 2.14.1 commonInterface.cpp adds this warning only after successful
// begin/endEvaluate. This rule does NOT apply to options/state/tagging APIs.
inline bool streamlineEvaluationCompleted(sl::Result result) noexcept {
    return result == sl::Result::eOk || result == sl::Result::eWarnOutOfVRAM;
}
}
