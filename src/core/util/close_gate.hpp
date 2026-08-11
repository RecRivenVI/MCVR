#pragma once

#include <atomic>

namespace mcvr {

class CloseGate final {
  public:
    bool begin() noexcept { return !closed_.exchange(true, std::memory_order_acq_rel); }
    bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

  private:
    std::atomic<bool> closed_{false};
};

} // namespace mcvr
