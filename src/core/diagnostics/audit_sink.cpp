#include "audit_sink.hpp"
#ifdef _WIN32
#define AUDIT_EXPORT extern "C" __declspec(dllexport)
#else
#define AUDIT_EXPORT extern "C" __attribute__((visibility("default")))
#endif
AUDIT_EXPORT int mcvrInstallAuditSink(const McvrAuditSink *sink) noexcept {
    return mcvr::audit::install(sink) ? 1 : 0;
}
