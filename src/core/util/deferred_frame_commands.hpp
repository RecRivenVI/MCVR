#pragma once

#include <memory>

namespace mcvr {
// The owner calls newFrame only after its frame fence has retired. Allocation is
// separate from recording because archived multi-view contexts retain this buffer.
template <class Buffer> class DeferredFrameCommands {
  public:
    template <class Factory> const std::shared_ptr<Buffer> &storage(Factory &&factory) {
        if (!buffer_) buffer_ = factory();
        return buffer_;
    }
    template <class Factory> const std::shared_ptr<Buffer> &begin(Factory &&factory) {
        storage(factory);
        if (!active_) {
            buffer_->begin();
            active_ = true; // Failed begin must never enter a submission list.
        }
        return buffer_;
    }
    void newFrame() noexcept { active_ = false; }
    bool active() const noexcept { return active_; }
    const std::shared_ptr<Buffer> &buffer() const noexcept { return buffer_; }
    void end() { if (active_) buffer_->end(); }
  private:
    std::shared_ptr<Buffer> buffer_;
    bool active_ = false;
};
}
