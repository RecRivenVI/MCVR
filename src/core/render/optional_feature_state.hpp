#pragma once

namespace mcvr {

enum class OptionalFeatureLoadAction {
    NoChange,
    InvokeRuntime,
    SatisfiedWithoutRuntime,
    Unavailable,
};

enum class OptionalFeatureLoadResult {
    Unchanged,
    Changed,
    SatisfiedWithoutRuntime,
    Unavailable,
    Failed,
};

constexpr OptionalFeatureLoadAction planOptionalFeatureLoad(
    bool runtimeReady, bool entryPointReady, bool currentlyLoaded, bool requested) {
    if (!runtimeReady || !entryPointReady) {
        return requested ? OptionalFeatureLoadAction::Unavailable
                         : OptionalFeatureLoadAction::SatisfiedWithoutRuntime;
    }
    return currentlyLoaded == requested ? OptionalFeatureLoadAction::NoChange
                                        : OptionalFeatureLoadAction::InvokeRuntime;
}

constexpr bool optionalFeatureRequestSatisfied(OptionalFeatureLoadResult result) {
    return result == OptionalFeatureLoadResult::Unchanged ||
           result == OptionalFeatureLoadResult::Changed ||
           result == OptionalFeatureLoadResult::SatisfiedWithoutRuntime;
}

} // namespace mcvr
