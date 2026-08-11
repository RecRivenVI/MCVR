#pragma once

#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>

namespace mcvr::diagnostics::lifecycle {

struct Snapshot {
    uint64_t submitAttempts = 0;
    uint64_t successfulSubmits = 0;
    uint64_t attemptsAtInjection = 0;
    uint64_t successesAtInjection = 0;
    uint64_t injections = 0;
    uint64_t rejectedProbeBodies = 0;
    uint64_t closeCalls = 0;
};

inline std::atomic<uint64_t> submitAttempts{0};
inline std::atomic<uint64_t> successfulSubmits{0};
inline std::atomic<uint64_t> attemptsAtInjection{0};
inline std::atomic<uint64_t> successesAtInjection{0};
inline std::atomic<uint64_t> injections{0};
inline std::atomic<uint64_t> rejectedProbeBodies{0};
inline std::atomic<uint64_t> closeCalls{0};

inline void noteSubmitAttempt() noexcept {
    submitAttempts.fetch_add(1, std::memory_order_relaxed);
}

inline void noteSuccessfulSubmit() noexcept {
    successfulSubmits.fetch_add(1, std::memory_order_relaxed);
}

inline void noteInjection() noexcept {
    attemptsAtInjection.store(submitAttempts.load(std::memory_order_acquire),
                              std::memory_order_release);
    successesAtInjection.store(successfulSubmits.load(std::memory_order_acquire),
                               std::memory_order_release);
    injections.fetch_add(1, std::memory_order_release);
}

inline void noteRejectedProbeBody() noexcept {
    // A non-zero value means JNI preflight incorrectly allowed this body to run.
    rejectedProbeBodies.fetch_add(1, std::memory_order_release);
}

inline void noteClose() noexcept {
    closeCalls.fetch_add(1, std::memory_order_release);
}

inline Snapshot snapshot() noexcept {
    return {
        submitAttempts.load(std::memory_order_acquire),
        successfulSubmits.load(std::memory_order_acquire),
        attemptsAtInjection.load(std::memory_order_acquire),
        successesAtInjection.load(std::memory_order_acquire),
        injections.load(std::memory_order_acquire),
        rejectedProbeBodies.load(std::memory_order_acquire),
        closeCalls.load(std::memory_order_acquire),
    };
}

inline std::string describe() {
    const auto value = snapshot();
    const uint64_t postInjectionAttempts = value.injections == 0
        ? 0 : value.submitAttempts - value.attemptsAtInjection;
    const uint64_t postInjectionSuccesses = value.injections == 0
        ? 0 : value.successfulSubmits - value.successesAtInjection;
    std::ostringstream output;
    output << "submitAttempts=" << value.submitAttempts
           << " successfulSubmits=" << value.successfulSubmits
           << " attemptsAtInjection=" << value.attemptsAtInjection
           << " successesAtInjection=" << value.successesAtInjection
           << " postInjectionSubmitAttempts="
           << postInjectionAttempts
           << " postInjectionSuccessfulSubmits="
           << postInjectionSuccesses
           << " injections=" << value.injections
           << " rejectedProbeBodies=" << value.rejectedProbeBodies
           << " closeCalls=" << value.closeCalls;
    return output.str();
}

inline void resetForTest() noexcept {
    submitAttempts.store(0, std::memory_order_relaxed);
    successfulSubmits.store(0, std::memory_order_relaxed);
    attemptsAtInjection.store(0, std::memory_order_relaxed);
    successesAtInjection.store(0, std::memory_order_relaxed);
    injections.store(0, std::memory_order_relaxed);
    rejectedProbeBodies.store(0, std::memory_order_relaxed);
    closeCalls.store(0, std::memory_order_relaxed);
}

} // namespace mcvr::diagnostics::lifecycle
