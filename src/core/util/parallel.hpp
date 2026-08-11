#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace mcvr {

namespace detail {

template <typename Fn, typename Launcher>
void parallelForWithLauncher(size_t count, uint32_t threadCount, Fn &&fn, Launcher &&launcher) {
    if (count == 0) return;
    threadCount = std::min<uint32_t>(threadCount, static_cast<uint32_t>(count));
    if (threadCount <= 1 || count == 1) {
        for (size_t i = 0; i < count; ++i) fn(i);
        return;
    }

    std::atomic<size_t> nextIndex{0};
    std::atomic<bool> cancelled{false};
    std::exception_ptr firstException;
    std::mutex exceptionMutex;
    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    auto worker = [&]() {
        while (!cancelled.load(std::memory_order_acquire)) {
            const size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
            if (index >= count) break;
            try {
                fn(index);
            } catch (...) {
                {
                    std::lock_guard lock(exceptionMutex);
                    if (firstException == nullptr) firstException = std::current_exception();
                }
                cancelled.store(true, std::memory_order_release);
            }
        }
    };

    try {
        for (uint32_t i = 0; i < threadCount; ++i) {
            workers.emplace_back(launcher(worker));
        }
    } catch (...) {
        const auto creationFailure = std::current_exception();
        cancelled.store(true, std::memory_order_release);
        for (auto &thread : workers) {
            if (thread.joinable()) thread.join();
        }
        std::rethrow_exception(creationFailure);
    }

    for (auto &thread : workers) {
        if (thread.joinable()) thread.join();
    }
    if (firstException != nullptr) std::rethrow_exception(firstException);
}

} // namespace detail

inline uint32_t parallelThreadCount(const char *envName = "MCVR_SHADER_PACK_BUILD_THREADS") {
    const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    const char *rawValue = std::getenv(envName);
    if (rawValue != nullptr && rawValue[0] != '\0') {
        char *end = nullptr;
        long parsed = std::strtol(rawValue, &end, 10);
        if (end != rawValue && parsed > 0) {
            return static_cast<uint32_t>(std::min<long>(parsed, hardwareThreads));
        }
    }
    return std::min(8u, hardwareThreads);
}

template <typename Fn>
void parallelFor(size_t count, Fn &&fn, const char *envName = "MCVR_SHADER_PACK_BUILD_THREADS") {
    detail::parallelForWithLauncher(
        count, parallelThreadCount(envName), std::forward<Fn>(fn),
        [](auto worker) { return std::thread(std::move(worker)); });
}

} // namespace mcvr
