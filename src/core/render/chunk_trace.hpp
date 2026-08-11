#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <sstream>
namespace mcvr::chunkTrace {
inline const bool enabled = [] {
    const char *v = std::getenv("RADIANCE_CHUNK_TRACE");
    return v && v[0] == '1';
}();
inline uint64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct Event {
    uint64_t time;
    const char *stage;
    int64_t id, revision;
    uint64_t frame;
    uint32_t owner;
};
inline std::mutex mutex;
inline std::array<Event, 16384> events{};
inline size_t begin = 0, count = 0;
inline std::atomic<uint64_t> dropped{0};
inline std::atomic<uint64_t> serial{0};
inline void
note(const char *stage, int64_t id = -1, int64_t revision = -1, uint64_t frame = 0, uint32_t owner = 0) noexcept {
    if (!enabled) return;
    try {
        std::lock_guard lock(mutex);
        if (count == events.size()) {
            begin = (begin + 1) % events.size();
            --count;
            ++dropped;
        }
        events[(begin + count++) % events.size()] = {now(), stage, id, revision, frame, owner};
    } catch (...) { ++dropped; } // diagnostics never change submission/error handling
}
inline std::string drain() {
    if (!enabled) return "disabled";
    std::lock_guard lock(mutex);
    std::ostringstream out;
    out << "clock_ns=" << now() << ";dropped=" << dropped.load() << '\n';
    for (size_t i = 0; i < count; i++) {
        const auto &e = events[(begin + i) % events.size()];
        out << e.time << ',' << e.stage << ',' << e.id << ',' << e.revision << ',' << e.frame << ',' << e.owner << '\n';
    }
    count = begin = 0;
    return out.str();
}
} // namespace mcvr::chunkTrace
