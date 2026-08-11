#pragma once

#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace mcvr {
struct SceneReleasePolicy {
    bool descriptors = true;
    bool reconstruction = true;
};

constexpr SceneReleasePolicy sceneReleasePolicy(bool isolated, bool keepDescriptors, bool keepReconstruction) {
    return {!isolated || !keepDescriptors, !isolated || !keepReconstruction};
}

// Local attribution experiments only. An ordinary/Prism process cannot activate the
// comparisons merely by inheriting an environment variable; it lacks the test marker.
inline SceneReleasePolicy configuredSceneReleasePolicy() {
    std::error_code error;
    const bool isolated = std::filesystem::is_regular_file(".radiance-audit-test-instance", error);
    const auto requested = [](const char *name) {
        const char *value = std::getenv(name);
        return value && std::string_view(value) == "1";
    };
    return sceneReleasePolicy(isolated,
        requested("RADIANCE_KEEP_SCENE_DESCRIPTORS_ON_LEAVE"),
        requested("RADIANCE_KEEP_RECONSTRUCTION_ON_LEAVE"));
}
} // namespace mcvr
