#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>
namespace mcvr::chunkScheduling {
using Clock = std::chrono::steady_clock;
inline constexpr auto backgroundWait = std::chrono::milliseconds(25);
inline constexpr auto fairnessAge = std::chrono::milliseconds(250);
inline constexpr uint64_t uploadBudget = 8ull * 1024 * 1024;
// Bounds queued GPU build inputs separately from the per-poll preparation budget.
// AS/scratch allocations have an additional bound from the existing in-flight fence count.
inline constexpr uint64_t inFlightInputBudget = 32ull * 1024 * 1024;
inline constexpr auto preparationBudget = std::chrono::milliseconds(2);
struct Key {
    int priority;
    Clock::time_point queued;
    double distance;
};
inline bool before(const Key &a, const Key &b, Clock::time_point now) {
    bool agedA = now - a.queued >= fairnessAge, agedB = now - b.queued >= fairnessAge;
    if (agedA != agedB) return agedA;
    if (agedA && a.queued != b.queued) return a.queued < b.queued;
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.distance != b.distance ? a.distance < b.distance : a.queued < b.queued;
}
inline bool ready(bool interactive, uint32_t count, uint32_t maximum, Clock::duration waited) {
    return interactive || count >= maximum || waited >= backgroundWait;
}
inline bool withinBudget(uint32_t admitted, uint64_t bytes, Clock::duration elapsed) {
    return admitted == 0 || (bytes < uploadBudget && elapsed < preparationBudget);
}
inline bool canAdmit(uint64_t inFlightBytes, uint64_t bytes) {
    // A single oversized section can progress only when no other batch is in flight.
    return inFlightBytes == 0 || (inFlightBytes < inFlightInputBudget && bytes <= inFlightInputBudget - inFlightBytes);
}

/** Production selection: bounded top-K per lane; aging never buries interaction under an old backlog. */
template <class Range, class KeyOf, class BytesOf>
std::vector<int64_t> selectBatch(const Range &queue,
                                 size_t maximum,
                                 Clock::time_point now,
                                 KeyOf key,
                                 BytesOf bytesOf,
                                 uint64_t inFlightBytes,
                                 uint64_t admissionSequence = 0) {
    std::vector<int64_t> interactive, background, selected;
    interactive.reserve(maximum);
    background.reserve(maximum);
    selected.reserve(maximum);
    if (maximum == 0) return selected;
    auto less = [&](int64_t a, int64_t b) { return before(key(a), key(b), now); };
    for (int64_t id : queue) {
        auto &lane = key(id).priority > 0 ? interactive : background;
        if (lane.size() < maximum) {
            lane.push_back(id);
            std::push_heap(lane.begin(), lane.end(), less);
        } else if (less(id, lane.front())) {
            std::pop_heap(lane.begin(), lane.end(), less);
            lane.back() = id;
            std::push_heap(lane.begin(), lane.end(), less);
        }
    }
    std::sort_heap(interactive.begin(), interactive.end(), less);
    std::sort_heap(background.begin(), background.end(), less);
    size_t i = 0, b = 0;
    uint64_t bytes = 0;
    while (selected.size() < maximum && (i < interactive.size() || b < background.size())) {
        const bool agedBackground = b < background.size() && now - key(background[b]).queued >= fairnessAge;
        // Persisted across batches, including byte-limited/one-section batches. An aged background
        // owner gets every fourth admitted slot; three slots remain available to interaction.
        const bool takeBackground =
            i == interactive.size() || (agedBackground && (admissionSequence + selected.size()) % 4 == 3);
        const auto id = takeBackground ? background[b] : interactive[i];
        const uint64_t next = bytesOf(id);
        if ((!selected.empty() && next > uploadBudget - std::min(bytes, uploadBudget)) ||
            !canAdmit(inFlightBytes, bytes + next))
            break;
        bytes += next;
        selected.push_back(id);
        if (takeBackground)
            ++b;
        else
            ++i;
    }
    return selected;
}
} // namespace mcvr::chunkScheduling
