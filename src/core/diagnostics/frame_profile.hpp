#pragma once
#include "profile_api.h"
#include <atomic>
#include <chrono>
#include <algorithm>
#include <optional>

namespace mcvr::profile {
inline std::atomic<const McvrProfileSink *> sink{nullptr};
inline std::atomic<bool> active{false};
inline std::atomic<uint64_t> epoch{0};
inline thread_local uint64_t frame = 0;
inline bool enabled() noexcept { return active.load(std::memory_order_relaxed); }
inline bool install(const McvrProfileSink *value) noexcept {
    if (!value || value->abi != MCVR_PROFILE_ABI || value->size != sizeof(McvrProfileSink) || !value->sample) return false;
    const McvrProfileSink *empty = nullptr;
    return sink.compare_exchange_strong(empty, value) || empty == value;
}
inline void emit(uint64_t owner, uint32_t domain, const char *label, uint64_t inclusive, uint64_t self) noexcept {
    if (auto observer = sink.load(std::memory_order_acquire)) observer->sample(owner, domain, label, inclusive, self);
}
// Injectable clock also exercises the production nesting/exception path in tests.
inline uint64_t now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Aggregate repeated geometry phases before crossing the optional observer boundary.
// Individual Scope instances still account for nesting in their enclosing operation.
struct Accumulated {
    const char *label;
    uint64_t inclusive = 0, self = 0, owner = frame, generation = epoch.load();
    bool recording = enabled();
    explicit Accumulated(const char *name) noexcept : label(name) {}
    ~Accumulated() noexcept {
        if (recording && enabled() && generation == epoch.load())
            emit(owner, owner ? 0 : 1, label, inclusive, self);
    }
};
class Scope {
    inline static thread_local Scope *top = nullptr;
    Scope *parent = nullptr;
    const char *label;
    uint64_t began = 0, children = 0, owner = 0, generation = 0;
    uint64_t (*clock)() noexcept;
    bool recording;
    Accumulated *accumulated = nullptr;
public:
    explicit Scope(const char *name, uint64_t (*time)() noexcept = now) noexcept
        : label(name), clock(time), recording(enabled()) {
        if (!recording) return;
        owner = frame; generation = epoch.load(); parent = top; began = clock(); top = this;
    }
    explicit Scope(Accumulated &target, uint64_t (*time)() noexcept = now) noexcept
        : Scope(target.label, time) { accumulated = &target; }
    Scope(const Scope &) = delete;
    ~Scope() noexcept {
        if (!recording) return;
        const auto elapsed = clock() - began;
        top = parent;
        if (parent) parent->children += elapsed;
        if (enabled() && generation == epoch.load()) {
            const auto self = elapsed - std::min(elapsed, children);
            if (accumulated) {
                accumulated->inclusive += elapsed;
                accumulated->self += self;
            } else emit(owner, owner ? 0 : 1, label, elapsed, self);
        }
    }
};
// Consecutive phases of one operation. Stack storage only; each transition closes
// the old scope before opening the next, preserving normal nesting and unwinding.
class Phases {
    std::optional<Scope> current;
    uint64_t (*clock)() noexcept;
public:
    explicit Phases(const char *name, uint64_t (*time)() noexcept = now) noexcept : clock(time) {
        current.emplace(name, clock);
    }
    explicit Phases(Accumulated &target, uint64_t (*time)() noexcept = now) noexcept : clock(time) {
        current.emplace(target, clock);
    }
    void next(Accumulated &target) noexcept {
        current.reset();
        current.emplace(target, clock);
    }
    void next(const char *name) noexcept {
        current.reset();
        current.emplace(name, clock);
    }
};
}
