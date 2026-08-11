#include "core/render/frame_retention.hpp"

#include <stdexcept>
#include <vector>

using mcvr::FrameRetention;

namespace {
void require(bool value, const char *what) {
    if (!value) throw std::runtime_error(what);
}

size_t count(const std::vector<FrameRetention<int>::Group> &groups) {
    size_t total = 0;
    for (const auto &group : groups) total += group.resources.size();
    return total;
}
} // namespace

int main() {
    constexpr uint32_t inFlight = 3;

    // Normal rotation: a frame's group lives until `inFlight` newer frames have begun (when the
    // per-image list of the same image was cleared) and only once its GPU work is known complete.
    {
        FrameRetention<int> retention;
        retention.retain(0); // before the first frame: group 0
        require(retention.beginFrame(1, 0, inFlight).empty(), "the pre-frame group waits for newer frames");
        retention.retain(10);
        require(retention.beginFrame(2, 0, inFlight).empty(), "nothing complete yet");
        require(count(retention.beginFrame(3, 1, inFlight)) == 1, "pre-frame group released at frame 3");
        retention.retain(30);
        require(count(retention.beginFrame(4, 1, inFlight)) == 1, "frame 1 released when frame 4 begins");
        require(retention.beginFrame(5, 3, inFlight).size() == 1, "frame 2 released; frame 3 waits for frame 6");
        require(retention.groups() == 3 && retention.oldestResources() == 1, "frames 3 (holding 30), 4 and 5 remain");
    }

    // Incomplete GPU work is never released, however many frames begin.
    {
        FrameRetention<int> retention;
        retention.beginFrame(1, 0, inFlight);
        retention.retain(1);
        for (uint64_t frame = 2; frame < 100; ++frame)
            require(retention.beginFrame(frame, 0, inFlight).empty(), "no completion, no release");
        require(retention.resources() == 1 && retention.oldestAge() == 98, "held while incomplete");
        require(count(retention.beginFrame(100, 1, inFlight)) == 1, "released once frame 1 completes");
    }

    // The failure the per-image lists had: the presentation engine returns only two of three images.
    // Retention of the frame that last used the third image (for example the frame retiring every
    // chunk after a teleport) must still be released once later frames complete.
    {
        FrameRetention<int> retention;
        retention.beginFrame(1, 0, inFlight); // image 0
        for (int i = 0; i < 8000; ++i) retention.retain(i);
        size_t released = 0;
        for (uint64_t frame = 2; frame < 50; ++frame) {
            // Images alternate 1, 2, 1, 2 ...; each acquire proves the frame two back complete.
            released += count(retention.beginFrame(frame, frame - 2, inFlight));
            retention.retain(int(frame));
        }
        require(released >= 8000, "the teleport frame's retention is released");
        require(retention.groups() <= inFlight + 1, "held groups stay bounded");
        require(retention.oldestAge() <= inFlight, "nothing older than the in-flight window stays");
    }

    // Swapchain recreation with an idle device drops everything.
    {
        FrameRetention<int> retention;
        retention.beginFrame(1, 0, inFlight);
        retention.retain(1);
        retention.beginFrame(2, 0, inFlight);
        retention.retain(2);
        require(count(retention.clear()) == 2 && retention.groups() == 0 && retention.oldestAge() == 0, "clear");
    }
    return 0;
}
