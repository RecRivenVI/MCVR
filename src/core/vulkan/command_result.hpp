#pragma once

#include "core/failure_state.hpp"
#include <utility>

namespace vk {
// Begin/end/reset are prerequisites for recording or submitting a command buffer.
// A failed prerequisite must never return a usable buffer to its caller.
template<class Operation, class Report>
void checkedCommandOperation(const char *name, Operation &&operation, Report &&report) {
    mcvr::failure::throwIfFatal();
    const VkResult result = std::forward<Operation>(operation)();
    if (result != VK_SUCCESS) {
        std::forward<Report>(report)(result, name);
        mcvr::failure::throwIfFatal();
        mcvr::failure::raise(mcvr::failure::Kind::runtime, result, name);
    }
    mcvr::failure::throwIfFatal();
}
} // namespace vk
