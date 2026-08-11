#include "core/diagnostics/alloc_trace.hpp"
#include "core/diagnostics/fg_timing.hpp"
#include <stdexcept>
static int creates = 0, destroys = 0, timings = 0;
static void allocation(int created, const char *, uint64_t, const char *) { created ? ++creates : ++destroys; }
static void timing(uint32_t, double) { ++timings; }
static int wants() { return 0; }
static void frame(const McvrAuditFrame *) {}
static void require(bool value) { if (!value) throw std::runtime_error("audit sink contract"); }
int main() {
    require(mcvr::diag::recordAllocCreate("buffer", 100) == nullptr);
    require(mcvr::fgdiag::start() == mcvr::fgdiag::Clock::time_point{});
    McvrAuditSink value{MCVR_AUDIT_ABI + 1, sizeof(McvrAuditSink), 3, allocation, timing, wants, frame};
    require(!mcvr::audit::install(&value));
    value.abi = MCVR_AUDIT_ABI;
    require(mcvr::audit::install(&value));
    require(mcvr::audit::install(&value));
    auto other = value;
    require(!mcvr::audit::install(&other));
    auto tag = mcvr::diag::recordAllocCreate("buffer", 100);
    mcvr::diag::recordAllocDestroy("buffer", 100, tag);
    mcvr::diag::recordAllocDestroy("pre-attach", 100, nullptr);
    mcvr::fgdiag::add(mcvr::fgdiag::MainAcquire, 2);
    require(creates == 1 && destroys == 1 && timings == 1);
    require(!mcvr::audit::enabled(MCVR_AUDIT_LIFECYCLE));
}
