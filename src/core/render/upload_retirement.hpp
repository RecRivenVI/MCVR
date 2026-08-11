#pragma once

#include "core/failure_state.hpp"

namespace mcvr::render {

// A failed poll must stop the caller before it records or submits more uploads.
// Keep the failed and unpolled batches owned until the normal fatal close path.
template<class Batches, class Poll, class Recycle, class Report>
void retireUploads(Batches &batches, Poll poll, Recycle recycle, Report report) {
    mcvr::failure::throwIfFatal();
    for (auto it = batches.begin(); it != batches.end();) {
        const VkResult result = poll(*it);
        if (result == VK_SUCCESS) {
            recycle(*it);
            it = batches.erase(it);
        } else if (result == VK_NOT_READY) {
            ++it;
        } else {
            report(result);
            mcvr::failure::throwIfFatal();
            mcvr::failure::raise(mcvr::failure::Kind::runtime, result, "texture upload retirement");
        }
    }
}

} // namespace mcvr::render
