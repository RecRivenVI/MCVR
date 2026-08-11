#pragma once
#include <stdint.h>

/* Optional diagnostic ABI. No STL/JNI/Vulkan ownership crosses this interface.
 * Install once. The provider and callbacks must live until process termination;
 * dynamic unloading is forbidden because GPU/resource destruction can report late events.
 * All callbacks must be thread-safe, non-throwing and must not re-enter rendering.
 * Strings are borrowed for the duration of a call. */
#define MCVR_AUDIT_ABI 1u
#define MCVR_AUDIT_ALLOCATIONS 1u
#define MCVR_AUDIT_TIMING 2u
#define MCVR_AUDIT_CAPTURE 4u
#define MCVR_AUDIT_LIFECYCLE 8u

struct McvrAuditFrame {
    uint64_t allocationBytes, allocationCount;
    uint32_t width, height, images;
    int32_t presentResult;
};
struct McvrAuditSink {
    uint32_t abi, size, flags;
    void (*allocation)(int created, const char *kind, uint64_t bytes, const char *tag);
    void (*timing)(uint32_t metric, double milliseconds);
    int (*wantsFrame)();
    void (*frame)(const struct McvrAuditFrame *frame);
};
typedef int (*McvrInstallAuditSink)(const struct McvrAuditSink *sink);
