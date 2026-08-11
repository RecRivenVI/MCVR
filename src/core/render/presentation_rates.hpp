#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace mcvr {
// Counts accepted Vulkan presents, not monitor scanout. Kept independent of
// temporary diagnostics. Written by the render thread, read by the debug HUD.
class PresentationRates {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_{};
    uint32_t real_ = 0, generated_ = 0;
    std::atomic<uint64_t> snapshot_{0};
public:
    void reset() {
        start_ = {}; real_ = generated_ = 0;
        snapshot_.store(0, std::memory_order_relaxed);
    }
    void record(bool realAccepted, bool generatedAccepted) { recordCounts(realAccepted, uint32_t(realAccepted)+uint32_t(generatedAccepted)); }
    void recordCounts(uint32_t realAccepted, uint32_t outputCount) {
        const auto now = Clock::now();
        if (start_ == Clock::time_point{}) { start_ = now; }
        real_ += realAccepted; generated_ += outputCount;
        const double seconds = std::chrono::duration<double>(now - start_).count();
        if (seconds < 1.0) return;
        const auto realFps = uint32_t(std::lround(real_ / seconds));
        const auto generatedFps = uint32_t(std::lround((generated_ > real_ ? generated_ - real_ : 0) / seconds));
        snapshot_.store((uint64_t(realFps) << 32) | generatedFps, std::memory_order_relaxed);
        real_ = generated_ = 0; start_ = now;
    }
    uint64_t snapshot() const { return snapshot_.load(std::memory_order_relaxed); }
};
inline PresentationRates presentationRates;
}
