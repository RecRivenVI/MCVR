#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace mcvr::diagnostics::device_loss {

struct Event {
    uint64_t sequence = 0;
    int64_t timeMicros = 0;
    const char *operation = nullptr;
    int32_t result = 0;
    uint32_t value0 = 0;
    uint32_t value1 = 0;
};

struct Slot {
    std::atomic<uint64_t> sequence{UINT64_MAX};
    std::atomic<int64_t> timeMicros{0};
    std::atomic<const char *> operation{nullptr};
    std::atomic<int32_t> result{0};
    std::atomic<uint32_t> value0{0};
    std::atomic<uint32_t> value1{0};
};

inline constexpr size_t capacity = 256;
inline std::array<Slot, capacity> events{};
inline std::atomic<uint64_t> nextSequence{0};

// Stable numeric fingerprint: event entries never borrow a pass-name string.
constexpr uint32_t passFingerprint(std::string_view name) noexcept {
    uint32_t value = 2166136261u;
    for (unsigned char byte : name) value = (value ^ byte) * 16777619u;
    return value;
}

inline bool enabled() noexcept {
    static const bool value = [] {
        const char *flag = std::getenv("MCVR_DEVICE_LOSS_TRACE");
        return flag != nullptr && flag[0] == '1' && flag[1] == '\0';
    }();
    return value;
}

inline void note(const char *operation, VkResult result = VK_SUCCESS,
                 uint32_t value0 = 0, uint32_t value1 = 0) noexcept {
    if (!enabled()) return;
    const uint64_t sequence = nextSequence.fetch_add(1, std::memory_order_relaxed);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto &slot = events[sequence % capacity];
    slot.sequence.store(UINT64_MAX, std::memory_order_relaxed);
    slot.timeMicros.store(micros, std::memory_order_relaxed);
    slot.operation.store(operation, std::memory_order_relaxed);
    slot.result.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    slot.value0.store(value0, std::memory_order_relaxed);
    slot.value1.store(value1, std::memory_order_relaxed);
    slot.sequence.store(sequence, std::memory_order_release);
}

inline void dump(const char *cause, VkResult result) noexcept {
    if (!enabled()) return;
    try {
        note(cause, result);
        const uint64_t end = nextSequence.load(std::memory_order_acquire);
        const uint64_t begin = end > capacity ? end - capacity : 0;
        std::ofstream output("radiance-device-loss-trace.log", std::ios::app);
        if (!output) return;
        output << "BEGIN cause=" << cause << " result=" << static_cast<int32_t>(result)
               << " events=" << (end - begin) << '\n';
        for (uint64_t sequence = begin; sequence < end; ++sequence) {
            auto &slot = events[sequence % capacity];
            if (slot.sequence.load(std::memory_order_acquire) != sequence) continue;
            const Event event{sequence, slot.timeMicros.load(std::memory_order_relaxed),
                slot.operation.load(std::memory_order_relaxed),
                slot.result.load(std::memory_order_relaxed),
                slot.value0.load(std::memory_order_relaxed),
                slot.value1.load(std::memory_order_relaxed)};
            if (event.operation == nullptr || slot.sequence.load(std::memory_order_acquire) != sequence) continue;
            output << event.sequence << ' ' << event.timeMicros << ' '
                   << event.operation << " result=" << event.result
                   << " value0=" << event.value0 << " value1=" << event.value1 << '\n';
        }
        output << "END\n";
    } catch (...) {
        // Fatal publication has already happened. Diagnostic I/O is best effort.
    }
}

inline void resetForTest() noexcept {
    nextSequence.store(0, std::memory_order_relaxed);
    for (auto &slot : events) {
        slot.sequence.store(UINT64_MAX, std::memory_order_relaxed);
        slot.operation.store(nullptr, std::memory_order_relaxed);
    }
}

inline std::vector<Event> snapshotForTest() {
    std::vector<Event> result;
    const uint64_t end = nextSequence.load(std::memory_order_acquire);
    const uint64_t begin = end > capacity ? end - capacity : 0;
    for (uint64_t sequence = begin; sequence < end; ++sequence) {
        auto &slot = events[sequence % capacity];
        if (slot.sequence.load(std::memory_order_acquire) != sequence) continue;
        Event event{sequence, slot.timeMicros.load(std::memory_order_relaxed),
            slot.operation.load(std::memory_order_relaxed), slot.result.load(std::memory_order_relaxed),
            slot.value0.load(std::memory_order_relaxed), slot.value1.load(std::memory_order_relaxed)};
        if (event.operation != nullptr && slot.sequence.load(std::memory_order_acquire) == sequence)
            result.push_back(event);
    }
    return result;
}

} // namespace mcvr::diagnostics::device_loss
