#pragma once
#include <memory>
#include <vector>

namespace vk { class TLAS; }
class Buffers;
class World;
class FrameworkContext;

// Redirect legacy scene lookups only on the recording thread. GPU descriptors always
// reference the scene's own resources; the main world's objects are never replaced.
struct SceneRecordingState {
    std::shared_ptr<Buffers> buffers;
    std::shared_ptr<World> world;
    std::vector<std::shared_ptr<FrameworkContext>> contexts;
    std::shared_ptr<FrameworkContext> current;
    uint32_t viewCount = 1;
    // Cleared at batch start. All views use the same immutable instance set and origin.
    std::shared_ptr<vk::TLAS> batchTlas;
    uint32_t batchTlasBuilds = 0;
    bool resetHistory = false;
};

class SceneRecordingScope {
    inline static thread_local SceneRecordingState *active_ = nullptr;
    SceneRecordingState *previous_;
public:
    explicit SceneRecordingScope(SceneRecordingState &state) : previous_(active_) { active_ = &state; }
    ~SceneRecordingScope() { active_ = previous_; }
    SceneRecordingScope(const SceneRecordingScope &) = delete;
    SceneRecordingScope &operator=(const SceneRecordingScope &) = delete;
    static SceneRecordingState *active() { return active_; }
};
