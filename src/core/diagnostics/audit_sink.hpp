#pragma once
#include "audit_api.h"
#include <atomic>

namespace mcvr::audit {
inline std::atomic<const McvrAuditSink *> sink{nullptr};
inline bool install(const McvrAuditSink *value) noexcept {
    if (!value || value->abi != MCVR_AUDIT_ABI || value->size != sizeof(McvrAuditSink)
        || (value->flags & ~15u) || !value->allocation || !value->timing || !value->wantsFrame || !value->frame)
        return false;
    const McvrAuditSink *empty = nullptr;
    return sink.compare_exchange_strong(empty, value) || empty == value;
}
inline bool enabled(uint32_t feature) noexcept {
    auto value = sink.load(std::memory_order_acquire);
    return value && (value->flags & feature);
}
}
