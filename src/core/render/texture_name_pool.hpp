#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mcvr {

class TextureNamePool {
public:
    explicit TextureNamePool(uint32_t limit = 4096) : limit_(limit), inUse_(limit, false) {}

    uint32_t allocate() {
        if (!free_.empty()) {
            const uint32_t id = free_.back();
            free_.pop_back();
            inUse_[id] = true;
            return id;
        }
        if (next_ >= limit_) throw std::overflow_error("Texture descriptor capacity exhausted");
        const uint32_t id = next_++;
        inUse_[id] = true;
        return id;
    }

    void release(uint32_t id) {
        if (id == 0 || id >= next_ || !inUse_[id]) return;
        inUse_[id] = false;
        free_.push_back(id);
    }

    void reset() { next_ = 1; free_.clear(); std::fill(inUse_.begin(), inUse_.end(), false); }
    uint32_t highWatermark() const noexcept { return next_; }
    size_t freeCount() const noexcept { return free_.size(); }

private:
    uint32_t limit_;
    uint32_t next_ = 1;
    std::vector<uint32_t> free_;
    std::vector<bool> inUse_;
};

} // namespace mcvr
