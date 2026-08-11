#include "core/render/modules/world/dlss/fg_present_policy.hpp"
#include "core/render/frame_acquire_policy.hpp"
#include "core/render/optional_feature_state.hpp"
#include "core/render/modules/world/dlss/dlss_evaluate_state.hpp"
#include "core/failure_state.hpp"
#include "core/vulkan/command_result.hpp"
#include "core/diagnostics/lifecycle_acceptance.hpp"
#include "core/util/parallel.hpp"
#include "core/util/borrowed_string.hpp"
#include "core/util/close_gate.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {
void throwWhileSavingFailure(mcvr::failure::Snapshot &, mcvr::failure::Kind, VkResult,
                             std::string_view, std::string_view) {
    throw std::bad_alloc();
}

std::string readSource(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    assert(stream.is_open());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

size_t verifyJniBoundaries(const std::filesystem::path &root) {
    size_t exports = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp") continue;
        const auto text = readSource(entry.path());
        size_t cursor = 0;
        while ((cursor = text.find("JNIEXPORT", cursor)) != std::string::npos) {
            const auto call = text.find("JNICALL", cursor);
            assert(call != std::string::npos);
            const auto open = text.find('{', call);
            assert(open != std::string::npos);
            size_t end = open + 1;
            int depth = 1;
            for (; end < text.size() && depth != 0; ++end) {
                if (text[end] == '{') ++depth;
                if (text[end] == '}') --depth;
            }
            assert(depth == 0);
            const auto body = text.substr(open + 1, end - open - 2);
            assert(body.find("jni::invoke") != std::string::npos ||
                   body.find("guarded(") != std::string::npos);
            ++exports;
            cursor = end;
        }
    }
    return exports;
}
}

int main() {
    {
        bool failed=false, reset=false, surface=false;
        mcvr::dlss::applyAsyncPresentResult(VK_ERROR_OUT_OF_DATE_KHR,failed,reset,surface);
        assert(!failed && reset && surface);
        reset=false; surface=false;
        mcvr::dlss::applyAsyncPresentResult(VK_SUCCESS,failed,reset,surface);
        assert(!failed && !reset && !surface);
        mcvr::dlss::applyAsyncPresentResult(VK_ERROR_DEVICE_LOST,failed,reset,surface);
        assert(failed);
        mcvr::dlss::applyAsyncPresentResult(VK_SUBOPTIMAL_KHR,failed,reset,surface);
        assert(failed && reset && surface); // surface changes never clear an earlier fatal
    }

    mcvr::diagnostics::lifecycle::resetForTest();
    mcvr::diagnostics::lifecycle::noteSubmitAttempt();
    mcvr::diagnostics::lifecycle::noteSuccessfulSubmit();
    mcvr::diagnostics::lifecycle::noteInjection();
    mcvr::diagnostics::lifecycle::noteSubmitAttempt();
    mcvr::diagnostics::lifecycle::noteClose();
    const auto lifecycleSnapshot = mcvr::diagnostics::lifecycle::snapshot();
    assert(lifecycleSnapshot.submitAttempts == 2);
    assert(lifecycleSnapshot.successfulSubmits == 1);
    assert(lifecycleSnapshot.attemptsAtInjection == 1);
    assert(lifecycleSnapshot.successesAtInjection == 1);
    assert(lifecycleSnapshot.injections == 1);
    assert(lifecycleSnapshot.rejectedProbeBodies == 0);
    assert(lifecycleSnapshot.closeCalls == 1);
    const auto lifecycleDescription = mcvr::diagnostics::lifecycle::describe();
    assert(lifecycleDescription.find("postInjectionSubmitAttempts=1") != std::string::npos);
    assert(lifecycleDescription.find("postInjectionSuccessfulSubmits=0") != std::string::npos);

    mcvr::CloseGate closeGate;
    std::atomic<int> closeOwners{0};
    std::vector<std::thread> closeAttempts;
    for (int index = 0; index < 16; ++index) {
        closeAttempts.emplace_back([&] {
            if (closeGate.begin()) closeOwners.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto &thread : closeAttempts) thread.join();
    assert(closeOwners.load(std::memory_order_relaxed) == 1);
    assert(closeGate.closed());
    assert(!closeGate.begin());

    const char16_t nonTerminatedUtf16[] = {u'G', u'L', u'F', u'W', u'\u6d4b', u'\u8bd5'};
    int utf16Releases = 0;
    const auto utf16 = mcvr::detail::copyBorrowedString<char16_t>(
        1,
        [](int) { return 6; },
        [&](int) { return nonTerminatedUtf16; },
        [&](int, const char16_t *characters) {
            assert(characters == nonTerminatedUtf16);
            ++utf16Releases;
        },
        [] { return false; });
    assert(utf16.has_value());
    assert(*utf16 == std::u16string(nonTerminatedUtf16, 6));
    assert(utf16->c_str()[utf16->size()] == u'\0');
    assert(utf16Releases == 1);

    int pendingChecks = 0;
    int pendingReleases = 0;
    const auto pendingAfterAcquire = mcvr::detail::copyBorrowedString<char16_t>(
        1,
        [](int) { return 1; },
        [&](int) { return nonTerminatedUtf16; },
        [&](int, const char16_t *) { ++pendingReleases; },
        [&] { return ++pendingChecks == 3; });
    assert(!pendingAfterAcquire.has_value());
    assert(pendingReleases == 1);

    int failedAcquireReleases = 0;
    const auto failedAcquire = mcvr::detail::copyBorrowedString<char16_t>(
        1,
        [](int) { return 1; },
        [](int) -> const char16_t * { return nullptr; },
        [&](int, const char16_t *) { ++failedAcquireReleases; },
        [] { return false; });
    assert(!failedAcquire.has_value());
    assert(failedAcquireReleases == 0);

    int nullAcquires = 0;
    const auto nullValue = mcvr::detail::copyBorrowedString<char16_t>(
        0,
        [](int) { return 0; },
        [&](int) -> const char16_t * { ++nullAcquires; return nonTerminatedUtf16; },
        [](int, const char16_t *) {},
        [] { return false; });
    assert(!nullValue.has_value());
    assert(nullAcquires == 0);

    mcvr::failure::State detailFailure;
    detailFailure.record(mcvr::failure::Kind::runtime, VK_ERROR_OUT_OF_HOST_MEMORY,
                         "allocation without diagnostics", {}, &throwWhileSavingFailure);
    assert(detailFailure.fatal());
    assert(detailFailure.firstResult() == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(detailFailure.detailRecordingFailed());
    const auto minimalFailure = detailFailure.snapshot();
    assert(minimalFailure.fatal);
    assert(minimalFailure.result == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(!minimalFailure.operation.empty());

    mcvr::failure::clearForInitialization();
    assert(!mcvr::failure::snapshot().fatal);

    std::atomic<int> normalParallelCalls{0};
    mcvr::detail::parallelForWithLauncher(
        32, 4,
        [&](size_t) { normalParallelCalls.fetch_add(1, std::memory_order_relaxed); },
        [](auto worker) { return std::thread(std::move(worker)); });
    assert(normalParallelCalls.load(std::memory_order_relaxed) == 32);

    std::atomic<int> launchedWorkers{0};
    std::atomic<int> finishedWorkers{0};
    std::atomic<int> activeWorkers{0};
    int launchAttempts = 0;
    bool creationFailureReturned = false;
    try {
        mcvr::detail::parallelForWithLauncher(
            256, 4,
            [](size_t) { std::this_thread::yield(); },
            [&](auto worker) {
                if (++launchAttempts == 3) throw std::runtime_error("injected thread creation failure");
                launchedWorkers.fetch_add(1, std::memory_order_relaxed);
                return std::thread([&, worker = std::move(worker)]() mutable {
                    activeWorkers.fetch_add(1, std::memory_order_relaxed);
                    worker();
                    activeWorkers.fetch_sub(1, std::memory_order_relaxed);
                    finishedWorkers.fetch_add(1, std::memory_order_relaxed);
                });
            });
    } catch (const std::runtime_error &error) {
        creationFailureReturned = std::string_view(error.what()) == "injected thread creation failure";
    }
    assert(creationFailureReturned);
    assert(launchedWorkers.load(std::memory_order_relaxed) == 2);
    assert(finishedWorkers.load(std::memory_order_relaxed) == 2);
    assert(activeWorkers.load(std::memory_order_relaxed) == 0);

    std::atomic<int> parallelCalls{0};
    bool workerFailureReturned = false;
    try {
        mcvr::parallelFor(64, [&](size_t index) {
            parallelCalls.fetch_add(1, std::memory_order_relaxed);
            if (index == 0) throw std::runtime_error("injected worker failure");
        });
    } catch (const std::runtime_error &) {
        workerFailureReturned = true;
    }
    assert(workerFailureReturned);
    assert(parallelCalls.load(std::memory_order_relaxed) > 0);

    std::atomic<bool> beginConcurrentRecord{false};
    std::vector<std::thread> recorders;
    for (int index = 0; index < 32; ++index) {
        recorders.emplace_back([&, index] {
            while (!beginConcurrentRecord.load(std::memory_order_acquire)) std::this_thread::yield();
            const auto identity = "concurrent-" + std::to_string(index);
            const VkResult result = index == 31 ? VK_ERROR_DEVICE_LOST : VK_ERROR_UNKNOWN;
            mcvr::failure::record(index == 31 ? mcvr::failure::Kind::deviceLost
                                              : mcvr::failure::Kind::runtime,
                                  result, identity, identity);
        });
    }
    beginConcurrentRecord.store(true, std::memory_order_release);
    for (auto &thread : recorders) thread.join();
    const auto concurrentFailure = mcvr::failure::snapshot();
    assert(concurrentFailure.fatal);
    assert(!concurrentFailure.operation.empty());
    assert(concurrentFailure.operation == concurrentFailure.description);
    assert(mcvr::failure::isDeviceLost());
    mcvr::failure::clearForInitialization();
    mcvr::failure::record(mcvr::failure::Kind::initialization, VK_ERROR_INITIALIZATION_FAILED,
                          "test initialization");
    assert(!mcvr::failure::snapshot().fatal);
    mcvr::failure::record(mcvr::failure::Kind::runtime, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "test allocation");
    const auto firstFailure = mcvr::failure::snapshot();
    assert(firstFailure.fatal);
    assert(firstFailure.result == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(firstFailure.operation == "test allocation");
    assert(!mcvr::failure::isDeviceLost());
    mcvr::failure::record(mcvr::failure::Kind::deviceLost, VK_ERROR_DEVICE_LOST,
                          "later device loss");
    assert(mcvr::failure::snapshot().operation == "test allocation");
    assert(mcvr::failure::snapshot().result == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(mcvr::failure::isDeviceLost());
    mcvr::failure::State localOrdinaryFailure;
    localOrdinaryFailure.record(mcvr::failure::Kind::runtime, VK_ERROR_UNKNOWN,
                                "local ordinary failure");
    assert(!mcvr::failure::shouldWaitForGpuIdle(localOrdinaryFailure));
    bool rejectedAfterFatal = false;
    try { mcvr::failure::throwIfFatal(); }
    catch (const mcvr::failure::FatalError &) { rejectedAfterFatal = true; }
    assert(rejectedAfterFatal);
    mcvr::failure::clearForInitialization();
    assert(!mcvr::failure::snapshot().fatal);
    assert(!mcvr::failure::isDeviceLost());
    assert(mcvr::failure::shouldWaitForGpuIdle(localOrdinaryFailure));
    mcvr::failure::record(mcvr::failure::Kind::deviceLost, VK_ERROR_DEVICE_LOST,
                          "injected device loss");
    assert(mcvr::failure::isDeviceLost());
    mcvr::failure::clearForInitialization();

    // Same stage boundary as world modules/uploads: a callee can record an
    // error without throwing. No later render/submit stage may then execute.
    int completedStage = 0;
    mcvr::failure::runCheckedStage([&] { ++completedStage; });
    bool propagatedStageFailure = false;
    try {
        mcvr::failure::runCheckedStage([&] {
            mcvr::failure::record(mcvr::failure::Kind::deviceLost, VK_ERROR_DEVICE_LOST,
                "vkQueueSubmit(chunk build)");
        });
        mcvr::failure::runCheckedStage([&] { ++completedStage; });
    } catch (const mcvr::failure::FatalError &error) {
        propagatedStageFailure = true;
        assert(error.result() == VK_ERROR_DEVICE_LOST);
        assert(error.operation() == "vkQueueSubmit(chunk build)");
    }
    assert(propagatedStageFailure && completedStage == 1);
    try { mcvr::failure::runCheckedStage([&] { ++completedStage; }); }
    catch (const mcvr::failure::FatalError &) {}
    assert(completedStage == 1 && mcvr::failure::isDeviceLost());
    assert(mcvr::failure::snapshot().operation == "vkQueueSubmit(chunk build)");
    mcvr::failure::clearForInitialization();

    // The production begin/end/reset helper must stop chained recording/submission.
    for (const char *operation : {"vkBeginCommandBuffer", "vkEndCommandBuffer", "vkResetCommandBuffer"}) {
        int calls = 0, continuation = 0;
        auto report = [](VkResult result, const char *name) {
            mcvr::failure::record(mcvr::failure::Kind::runtime, result, name);
        };
        bool caught = false;
        try {
            vk::checkedCommandOperation(operation,
                [&] { ++calls; return VK_ERROR_OUT_OF_HOST_MEMORY; }, report);
            ++continuation;
        } catch (const mcvr::failure::FatalError &error) {
            caught = true;
            assert(error.result() == VK_ERROR_OUT_OF_HOST_MEMORY && error.operation() == operation);
        }
        assert(caught && calls == 1 && continuation == 0);
        try { vk::checkedCommandOperation(operation, [&] { ++calls; return VK_SUCCESS; }, report); }
        catch (const mcvr::failure::FatalError &) {}
        assert(calls == 1); // no Vulkan call after sticky fatal
        mcvr::failure::clearForInitialization();
        vk::checkedCommandOperation(operation, [&] { ++calls; return VK_SUCCESS; }, report);
        assert(calls == 2 && !mcvr::failure::globalState.fatal());
    }

    mcvr::FrameAcquireAttempt acquire;
    acquire.observe(VK_ERROR_OUT_OF_DATE_KHR);
    assert(!acquire.acquired());
    assert(acquire.requiresRecreate());
    assert(acquire.exhaustedResult() == VK_NOT_READY);
    acquire.observe(VK_SUBOPTIMAL_KHR);
    assert(acquire.acquired());
    assert(acquire.suboptimal());

    using enum mcvr::OptionalFeatureLoadAction;
    assert(mcvr::planOptionalFeatureLoad(false, false, true, false) == SatisfiedWithoutRuntime);
    assert(mcvr::planOptionalFeatureLoad(false, false, true, true) == Unavailable);
    assert(mcvr::planOptionalFeatureLoad(true, true, false, false) == NoChange);
    assert(mcvr::planOptionalFeatureLoad(true, true, false, true) == InvokeRuntime);
    assert(mcvr::optionalFeatureRequestSatisfied(
        mcvr::OptionalFeatureLoadResult::SatisfiedWithoutRuntime));
    assert(!mcvr::optionalFeatureRequestSatisfied(mcvr::OptionalFeatureLoadResult::Unavailable));
    assert(!mcvr::optionalFeatureRequestSatisfied(mcvr::OptionalFeatureLoadResult::Failed));

    DlssEvaluateState dlss;
    assert(!dlss.outputValid());
    assert(dlss.historyResetPending());
    dlss.begin();
    dlss.complete(true);
    assert(dlss.outputValid());
    assert(!dlss.historyResetPending());
    dlss.begin();
    dlss.complete(false);
    assert(!dlss.outputValid());
    assert(dlss.historyResetPending());
    dlss.completeFallback();
    assert(dlss.outputValid());
    assert(!dlss.dlssOutputValid());
    assert(dlss.fallbackOutputValid());
    assert(dlss.historyResetPending());

    const auto source = std::filesystem::path(MCVR_SOURCE_DIR);
    const std::regex processExit(R"(\b(exit|abort|terminate|quick_exit|_Exit)\s*\()",
                                 std::regex::ECMAScript);
    for (const auto &entry : std::filesystem::recursive_directory_iterator(source / "src")) {
        if (!entry.is_regular_file()) continue;
        const auto extension = entry.path().extension().string();
        if (extension != ".cpp" && extension != ".hpp" && extension != ".c" && extension != ".h") continue;
        assert(!std::regex_search(readSource(entry.path()), processExit));
    }
    const auto exports = verifyJniBoundaries(source / "src/core/middleware") +
                         verifyJniBoundaries(source / "src/core/loading");
    // Every discovered body is checked above. The separate generated-header/export comparison
    // verifies completeness; adding a guarded diagnostic entry does not change this contract.
    assert(exports > 0);
}
