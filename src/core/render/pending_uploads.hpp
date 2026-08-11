#pragma once
#include <memory>
#include <utility>
#include <vector>

namespace mcvr {
// Detach before recording. A later flush can only see newly queued payloads;
// the caller retains the detached batch until the submission retires.
template<class T>
auto takePendingUploads(std::shared_ptr<std::vector<T>> &pending) {
    return std::exchange(pending, std::make_shared<std::vector<T>>());
}
}
