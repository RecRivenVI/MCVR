#pragma once
#include <utility>
namespace mcvr {
// Recording is not submission. An abandoned frame must record the build again;
// only a successful queue submission permits retiring its build inputs/scratch.
class RigidBuildLifecycle {
    enum class State { Pending, Recorded, Submitted } state_ = State::Pending;
  public:
    void beginFrame() noexcept { if (state_ == State::Recorded) state_ = State::Pending; }
    template<class Record> bool record(Record &&record) {
        if (state_ != State::Pending) return false;
        std::forward<Record>(record)();
        state_ = State::Recorded;
        return true;
    }
    bool submitted() noexcept {
        if (state_ != State::Recorded) return false;
        state_ = State::Submitted;
        return true;
    }
};
}
