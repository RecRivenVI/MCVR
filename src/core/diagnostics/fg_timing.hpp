#pragma once
#include "audit_sink.hpp"
#include <chrono>
namespace mcvr::fgdiag {
using Clock = std::chrono::steady_clock;
enum Metric { MainAcquire, MainFence, MainSubmit, FirstPresent, ExtraFence,
    ExtraAcquire, ExtraSubmit, SecondPresent, Limiter, Record, GpuFrame,
    GpuPrepare, GpuEvaluate, Count };
inline Clock::time_point start() noexcept {
    return audit::enabled(MCVR_AUDIT_TIMING) ? Clock::now() : Clock::time_point{};
}
inline void add(Metric metric, double milliseconds) noexcept {
    auto sink = audit::sink.load(std::memory_order_acquire);
    if (sink && (sink->flags & MCVR_AUDIT_TIMING)) sink->timing(metric, milliseconds);
}
inline void elapsed(Metric metric, Clock::time_point began) noexcept {
    if (began != Clock::time_point{}) add(metric, std::chrono::duration<double,std::milli>(Clock::now()-began).count());
}
struct Scope {
    Metric metric;
    Clock::time_point began;
    explicit Scope(Metric value) noexcept : metric(value), began(start()) {}
    ~Scope() { elapsed(metric, began); }
};
}
