#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace mcvr {

// Frame-scoped retention of resources that GPU work may still reference. Each acquired frame opens a
// group, and everything retained until the next frame belongs to it. A group is released once a later
// completed frame proves its GPU work finished (fences on one queue signal in submission order) and at
// least `framesInFlight` newer frames have begun, the minimum the former per-swapchain-image lists
// gave in normal rotation. Keying by frame rather than by swapchain image means a group can no longer
// stay pinned while the presentation engine stops returning one image (observed alternating two of
// three images for a whole session, holding the teleport frame's retired chunks indefinitely).
template <class Resource>
class FrameRetention {
  public:
    struct Group {
        uint64_t serial = 0;
        std::vector<Resource> resources;
    };

    void retain(Resource resource) {
        if (groups_.empty()) groups_.push_back({});
        groups_.back().resources.push_back(std::move(resource));
    }

    // Opens the group for `frameSerial` (strictly increasing). Returns the released groups so the
    // caller can destroy them outside its lock.
    std::vector<Group> beginFrame(uint64_t frameSerial, uint64_t completedSerial, uint32_t framesInFlight) {
        std::vector<Group> released;
        while (!groups_.empty() && groups_.front().serial <= completedSerial &&
               groups_.front().serial + framesInFlight <= frameSerial) {
            released.push_back(std::move(groups_.front()));
            groups_.pop_front();
        }
        groups_.push_back({frameSerial, {}});
        return released;
    }

    // For a device that is idle (swapchain recreation): everything may go.
    std::vector<Group> clear() {
        std::vector<Group> released(std::make_move_iterator(groups_.begin()), std::make_move_iterator(groups_.end()));
        groups_.clear();
        return released;
    }

    size_t groups() const { return groups_.size(); }
    size_t resources() const {
        size_t count = 0;
        for (const auto &group : groups_) count += group.resources.size();
        return count;
    }
    // Frames between the oldest held group and the newest one; 0 when empty.
    uint64_t oldestAge() const { return groups_.empty() ? 0 : groups_.back().serial - groups_.front().serial; }
    size_t oldestResources() const { return groups_.empty() ? 0 : groups_.front().resources.size(); }

  private:
    std::deque<Group> groups_;
};

} // namespace mcvr
