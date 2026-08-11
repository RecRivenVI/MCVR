#pragma once

#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif

namespace mcvr::diagnostics::local_gpu_capture {
// Optional external JVM diagnostic agent, loaded explicitly before SERVICE creates
// Vulkan. MCVR never loads/distributes the SDK and never owns this borrowed module.
inline uint32_t flags() noexcept {
#ifdef _WIN32
    const char *value = std::getenv("MCVR_LOCAL_AFTERMATH");
    if (!value || value[0] != '1' || value[1] != '\0') return 0;
    auto module = GetModuleHandleW(L"radiance-aftermath-agent.dll");
    if (!module) return 0;
    using Function = uint32_t (*)(uint32_t);
    auto function = reinterpret_cast<Function>(GetProcAddress(module, "RadianceGpuCaptureFlags"));
    if (function) return function(1);
#endif
    return 0;
}

inline void beforeLostDeviceRelease() noexcept {
#ifdef _WIN32
    if (!flags()) return;
    auto module = GetModuleHandleW(L"radiance-aftermath-agent.dll");
    using Function = uint32_t (*)(uint32_t);
    auto function = reinterpret_cast<Function>(GetProcAddress(module, "RadianceGpuCaptureWait"));
    // Agent consumes this once per process. It polls dump status, never GPU idle.
    if (function) function(5000);
#endif
}
} // namespace mcvr::diagnostics::local_gpu_capture
