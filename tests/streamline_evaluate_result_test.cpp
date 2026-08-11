#include "core/render/streamline_evaluate_result.hpp"
#include <stdexcept>

int main() {
    if (!mcvr::streamlineEvaluationCompleted(sl::Result::eOk) ||
        !mcvr::streamlineEvaluationCompleted(sl::Result::eWarnOutOfVRAM))
        throw std::runtime_error("completed evaluation was rejected");
    for (int result = int(sl::Result::eErrorIO); result <= int(sl::Result::eErrorInvalidState); ++result)
        if (mcvr::streamlineEvaluationCompleted(static_cast<sl::Result>(result)))
            throw std::runtime_error("failed evaluation was accepted");
    if (mcvr::streamlineEvaluationCompleted(static_cast<sl::Result>(999)))
        throw std::runtime_error("unknown result was accepted");
}
