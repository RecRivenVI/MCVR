#include "audit_sink.hpp"
#include "frame_profile.hpp"
#ifdef _WIN32
#define AUDIT_EXPORT extern "C" __declspec(dllexport)
#else
#define AUDIT_EXPORT extern "C" __attribute__((visibility("default")))
#endif
AUDIT_EXPORT int mcvrInstallAuditSink(const McvrAuditSink *sink) noexcept {
    return mcvr::audit::install(sink) ? 1 : 0;
}
AUDIT_EXPORT int mcvrInstallProfileSink(const McvrProfileSink *sink) noexcept {
    return mcvr::profile::install(sink) ? 1 : 0;
}
AUDIT_EXPORT void mcvrProfileFrame(uint64_t frame, int active) noexcept {
    mcvr::profile::frame = frame;
    const bool enabled = active != 0 && mcvr::profile::sink.load() != nullptr;
    if (enabled && !mcvr::profile::active.load()) ++mcvr::profile::epoch;
    mcvr::profile::active.store(enabled, std::memory_order_relaxed);
}
