#pragma once
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace mcvr {
// Single-owner, bounded cache. Recording/building alone never makes GPU geometry reusable.
template <class T> class SubmittedGeometryCache {
    uint64_t revision_ = 0;
    std::shared_ptr<T> value_;
    bool submitted_ = false, used_ = false;
public:
    void beginFrame() {
        if (!used_) clear();
        used_ = false;
    }
    std::shared_ptr<T> find(uint64_t revision) {
        used_ = true;
        return revision != 0 && revision == revision_ && submitted_ ? value_ : nullptr;
    }
    void store(uint64_t revision, std::shared_ptr<T> value) {
        if (revision == 0 || !value) throw std::invalid_argument("Invalid cached geometry owner");
        revision_ = revision;
        value_ = std::move(value);
        submitted_ = false;
        used_ = true;
    }
    void submitted() { if (value_) submitted_ = true; }
    void clear() { revision_ = 0; value_.reset(); submitted_ = used_ = false; }
};
}
