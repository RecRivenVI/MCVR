#pragma once
#include "audit_sink.hpp"
#include <cstdint>

namespace mcvr::diag {
inline thread_local const char *allocTraceTag = nullptr;
struct AllocTraceTagScope {
    const char *previous;
    explicit AllocTraceTagScope(const char *tag) noexcept : previous(allocTraceTag) { allocTraceTag = tag; }
    ~AllocTraceTagScope() { allocTraceTag = previous; }
};
inline const char *recordAllocCreate(const char *kind, uint64_t bytes) noexcept {
    auto sink = audit::sink.load(std::memory_order_acquire);
    if (!sink || !(sink->flags & MCVR_AUDIT_ALLOCATIONS)) return nullptr;
    const char *tag = allocTraceTag ? allocTraceTag : "internal";
    sink->allocation(1, kind, bytes, tag);
    return tag;
}
inline void recordAllocDestroy(const char *kind, uint64_t bytes, const char *tag) noexcept {
    if (!tag) return; // Created before attachment: never subtract from the diagnostic inventory.
    auto sink = audit::sink.load(std::memory_order_acquire);
    if (sink) sink->allocation(0, kind, bytes, tag);
}
}
