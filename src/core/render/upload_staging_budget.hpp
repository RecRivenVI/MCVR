#pragma once

#include <cstddef>

namespace mcvr::render {

inline size_t replacementUploadCapacity(size_t usedBytes, size_t baseBytes) noexcept {
    return usedBytes < baseBytes ? baseBytes : usedBytes;
}

class UploadStagingBudget {
  public:
    explicit UploadStagingBudget(size_t limitBytes) : limitBytes_(limitBytes) {}

    bool tryRetain(size_t bytes) noexcept {
        if (bytes > limitBytes_ || retainedBytes_ > limitBytes_ - bytes) return false;
        retainedBytes_ += bytes;
        return true;
    }

    void take(size_t bytes) noexcept {
        retainedBytes_ = bytes > retainedBytes_ ? 0 : retainedBytes_ - bytes;
    }

    size_t retainedBytes() const noexcept { return retainedBytes_; }

  private:
    size_t limitBytes_;
    size_t retainedBytes_ = 0;
};

} // namespace mcvr::render
