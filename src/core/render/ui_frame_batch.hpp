#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace mcvr::ui {
// Shared by the JNI collection boundary and its fault-injection tests. Execution
// cannot observe partial membership or borrow a participant from an older frame.
class FrameBatch {
    std::vector<uint64_t> expected_, received_;
    bool open_ = false;
public:
    void begin(std::span<const uint64_t> views) {
        if (open_ || views.size() > 2) throw std::logic_error("Invalid UI PT batch boundary");
        expected_.assign(views.begin(), views.end());
        std::sort(expected_.begin(), expected_.end());
        if (std::adjacent_find(expected_.begin(), expected_.end()) != expected_.end() ||
            (!expected_.empty() && expected_[0] == 0)) throw std::invalid_argument("Invalid UI PT identities");
        received_.clear(); open_ = true;
    }
    void receive(uint64_t view) {
        if (!open_ || !std::binary_search(expected_.begin(), expected_.end(), view) ||
            std::find(received_.begin(), received_.end(), view) != received_.end())
            throw std::logic_error("Unexpected or repeated UI PT participant");
        received_.push_back(view);
    }
    template<class Execute> void finish(bool commit, Execute execute) {
        if (!open_) throw std::logic_error("UI PT batch already ended");
        open_ = false;
        if (!commit) return;
        if (received_.size() != expected_.size()) throw std::logic_error("UI PT frame is missing a participant");
        execute();
    }
};
}
