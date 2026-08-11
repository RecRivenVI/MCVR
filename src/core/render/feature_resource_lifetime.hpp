#pragma once

#include <utility>

namespace mcvr {
// SDK resources only exist after evaluation. A failed free retains the state so a later
// close can retry; a never-evaluated or already-released feature is a successful no-op.
class FeatureResourceLifetime {
  public:
    void evaluated() { allocated_ = true; }
    bool allocated() const { return allocated_; }

    template <class Free>
    bool release(Free &&free) {
        if (!allocated_) return true;
        if (!std::forward<Free>(free)()) return false;
        allocated_ = false;
        return true;
    }

  private:
    bool allocated_ = false;
};
} // namespace mcvr
