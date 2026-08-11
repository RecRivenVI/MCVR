#pragma once

#include <vulkan/vulkan_core.h>

#include <atomic>
#include <bit>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mcvr::failure {

enum class Kind : uint32_t {
    initialization,
    runtime,
    deviceLost,
    invariant,
};

class FatalError final : public std::runtime_error {
  public:
    FatalError(Kind kind, VkResult result, std::string operation, std::string message)
        : std::runtime_error(std::move(message)), kind_(kind), result_(result), operation_(std::move(operation)) {}

    Kind kind() const noexcept { return kind_; }
    VkResult result() const noexcept { return result_; }
    const std::string &operation() const noexcept { return operation_; }

  private:
    Kind kind_;
    VkResult result_;
    std::string operation_;
};

struct Snapshot {
    bool fatal = false;
    Kind kind = Kind::runtime;
    VkResult result = VK_SUCCESS;
    std::string operation;
    std::string description;
};

inline const char *kindName(Kind kind) noexcept {
    switch (kind) {
        case Kind::initialization: return "initialization";
        case Kind::runtime: return "runtime";
        case Kind::deviceLost: return "device-lost";
        case Kind::invariant: return "invariant";
    }
    return "unknown";
}

// The first cause is a diagnostic contract. Device loss is a separate,
// monotonic cleanup contract: a later VK_ERROR_DEVICE_LOST must not replace the
// first cause, but every owner of this device still has to observe the loss.
class State final {
  public:
    using DetailRecorder = void (*)(Snapshot &, Kind, VkResult, std::string_view, std::string_view);

    void clear() noexcept {
        deviceLost_.store(false, std::memory_order_relaxed);
        firstFailure_.store(0, std::memory_order_release);
        detailRecordingFailed_.store(false, std::memory_order_relaxed);
        try {
            std::lock_guard lock(mutex_);
            stored_ = {};
        } catch (...) {
            detailRecordingFailed_.store(true, std::memory_order_release);
        }
    }

    void record(Kind kind, VkResult result, std::string_view operation,
                std::string_view description = {}, DetailRecorder recorder = &storeDetails) noexcept {
        if (kind == Kind::initialization) return;
        const Kind actual = result == VK_ERROR_DEVICE_LOST ? Kind::deviceLost : kind;
        if (actual == Kind::deviceLost) deviceLost_.store(true, std::memory_order_release);

        uint64_t expected = 0;
        if (!firstFailure_.compare_exchange_strong(expected, pack(actual, result),
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) return;

        try {
            std::lock_guard lock(mutex_);
            recorder(stored_, actual, result, operation, description);
        } catch (...) {
            // The non-allocating first cause above is already visible. Preserve
            // that cause and publish that optional diagnostics were incomplete.
            detailRecordingFailed_.store(true, std::memory_order_release);
        }
    }

    bool fatal() const noexcept { return firstFailure_.load(std::memory_order_acquire) != 0; }
    bool isDeviceLost() const noexcept { return deviceLost_.load(std::memory_order_acquire); }
    bool detailRecordingFailed() const noexcept {
        return detailRecordingFailed_.load(std::memory_order_acquire);
    }

    VkResult firstResult() const noexcept {
        const uint64_t packed = firstFailure_.load(std::memory_order_acquire);
        return packed == 0 ? VK_SUCCESS : unpackResult(packed);
    }

    Kind firstKind() const noexcept {
        const uint64_t packed = firstFailure_.load(std::memory_order_acquire);
        return packed == 0 ? Kind::runtime : static_cast<Kind>((packed >> 32U) & kindMask);
    }

    Snapshot snapshot() const {
        const uint64_t packed = firstFailure_.load(std::memory_order_acquire);
        if (packed == 0) return {};

        Snapshot value;
        try {
            std::lock_guard lock(mutex_);
            value = stored_;
        } catch (...) {
            detailRecordingFailed_.store(true, std::memory_order_release);
        }
        value.fatal = true;
        value.kind = static_cast<Kind>((packed >> 32U) & kindMask);
        value.result = unpackResult(packed);
        if (value.operation.empty()) value.operation = "native renderer operation";
        if (value.description.empty()) {
            value.description = value.operation + " failed with VkResult=" + std::to_string(value.result);
        }
        return value;
    }

    void noteDetailFailure() noexcept {
        detailRecordingFailed_.store(true, std::memory_order_release);
    }

  private:
    static constexpr uint64_t presentBit = uint64_t{1} << 63U;
    static constexpr uint64_t kindMask = 0x7fffffffULL;

    static uint64_t pack(Kind kind, VkResult result) noexcept {
        return presentBit | (static_cast<uint64_t>(kind) << 32U) |
               static_cast<uint32_t>(result);
    }

    static VkResult unpackResult(uint64_t packed) noexcept {
        return static_cast<VkResult>(std::bit_cast<int32_t>(static_cast<uint32_t>(packed)));
    }

    static void storeDetails(Snapshot &target, Kind kind, VkResult result,
                             std::string_view operation, std::string_view description) {
        Snapshot value;
        value.fatal = true;
        value.kind = kind;
        value.result = result;
        value.operation.assign(operation);
        value.description = description.empty()
            ? value.operation + " failed with VkResult=" + std::to_string(result)
            : std::string(description);
        target = std::move(value);
    }

    std::atomic<uint64_t> firstFailure_{0};
    std::atomic<bool> deviceLost_{false};
    mutable std::atomic<bool> detailRecordingFailed_{false};
    mutable std::mutex mutex_;
    Snapshot stored_;
};

inline State globalState;

inline void clearForInitialization() noexcept { globalState.clear(); }

inline void record(Kind kind, VkResult result, std::string_view operation,
                   std::string_view description = {}) noexcept {
    globalState.record(kind, result, operation, description);
}

inline bool isDeviceLost() noexcept { return globalState.isDeviceLost(); }

inline Snapshot snapshot() { return globalState.snapshot(); }

inline bool shouldWaitForGpuIdle(const State &localState) noexcept {
    return !localState.isDeviceLost() && !globalState.isDeviceLost();
}

inline void throwIfFatal() {
    if (!globalState.fatal()) return;
    const auto value = globalState.snapshot();
    throw FatalError(value.kind, value.result, value.operation,
                     "renderer is in fatal state after " + value.description);
}

// Some lower-level Vulkan paths publish a fatal result and return normally.
// A caller must not interpret that return as permission to run the next stage.
template<class Stage>
inline void runCheckedStage(Stage &&stage) {
    throwIfFatal();
    std::forward<Stage>(stage)();
    throwIfFatal();
}

[[noreturn]] inline void raise(Kind kind, VkResult result, std::string_view operation) {
    const Kind actual = result == VK_ERROR_DEVICE_LOST ? Kind::deviceLost : kind;
    const std::string description = std::string(operation) + " failed with VkResult=" + std::to_string(result);
    throw FatalError(actual, result, std::string(operation), description);
}

[[noreturn]] inline void invariant(std::string_view operation, std::string_view description) {
    throw FatalError(Kind::invariant, VK_ERROR_UNKNOWN, std::string(operation),
                     std::string(operation) + ": " + std::string(description));
}

} // namespace mcvr::failure
