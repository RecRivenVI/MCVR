#include "core/render/world.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <fstream>
#include <memory>

#include "core/render/buffers.hpp"
#include "core/render/chunks.hpp"
#include "core/render/entities.hpp"
#include "core/render/instancing.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/failure_state.hpp"
#include "core/render/scene_release_policy.hpp"
#include "core/logging.hpp"

namespace {
void dumpReleasedSceneAllocator(const std::shared_ptr<vk::VMA> &vma) noexcept {
    // Local, bounded attribution only. Never dump on an ordinary/Prism process.
    try {
        std::error_code error;
        const char *census = std::getenv("RADIANCE_CHUNK_CENSUS");
        if (!vma || !census || std::string_view(census) != "1" ||
            !std::filesystem::is_regular_file(".radiance-audit-test-instance", error)) return;
        VmaTotalStatistics statistics{};
        vmaCalculateStatistics(vma->allocator(), &statistics);
        if (statistics.total.statistics.allocationCount > 10000) return;
        char *json = nullptr;
        vmaBuildStatsString(vma->allocator(), &json, VK_TRUE);
        const auto free = [allocator = vma->allocator()](char *text) { vmaFreeStatsString(allocator, text); };
        std::unique_ptr<char, decltype(free)> owned(json, free);
        static uint32_t sequence = 0; // render-thread-only world-unload boundary
        std::ofstream file("vma-scene-release-" + std::to_string(++sequence) + ".json");
        if (file && json) file << json;
    } catch (...) {
        std::fputs("World-release allocator diagnostic failed; scene cleanup was already completed\n", stderr);
    }
}
}

World::World(std::shared_ptr<Framework> framework)
    : chunks_(Chunks::create(framework)), entities_(Entities::create(framework)),
      instancing_(Instancing::create(framework)) {}

void World::resetFrame() {}

bool &World::shouldRender() {
    return shouldRenderWorld_;
}

std::shared_ptr<Chunks> World::chunks() {
    return chunks_;
}

std::shared_ptr<Entities> World::entities() {
    return entities_;
}

std::shared_ptr<Instancing> World::instancing() { return instancing_; }

void World::setCameraPos(glm::dvec3 cameraPos) {
    cameraPos_ = cameraPos;
}

glm::dvec3 World::getCameraPos() {
    return cameraPos_;
}

void World::releaseScene() {
    auto framework = Renderer::instance().framework();
    if (framework == nullptr) return;
    // The client acquires the next context at the end of the previous frame. It
    // may therefore have recorded uploads/world commands which have not reached a
    // queue yet: waiting for the device alone does not complete that work.
    if (auto context = framework->safeAcquireCurrentContext(); context && !context->frameSubmitted) {
        const VkResult flushed = framework->flushForReadback();
        if (flushed != VK_SUCCESS) {
            mcvr::failure::raise(mcvr::failure::Kind::runtime, flushed, "world unload recorded work");
        }
    }
    // Streamline may own asynchronous presentation work. This boundary runs only on
    // world unload, and must drain every actual GPU user before freeing SDK/descriptors.
    const VkResult idle = mcvr::StreamlineRuntime::get().ready()
        ? framework->waitDeviceIdle() : framework->waitRenderQueueIdle();
    if (idle != VK_SUCCESS) mcvr::failure::raise(mcvr::failure::Kind::runtime, idle, "world unload GPU wait");
    const VkResult backend = framework->waitBackendQueueIdle();
    if (backend != VK_SUCCESS) mcvr::failure::raise(mcvr::failure::Kind::runtime, backend, "world unload backend wait");
    shouldRenderWorld_ = false;
    chunks_->releaseScene();
    entities_->releaseScene();
    if (auto pipeline = framework->pipeline(); pipeline != nullptr) pipeline->releaseWorldScene();
    const auto context = framework->safeAcquireCurrentContext();
    framework->frameResourceRetainer().clearAfterGpuIdle(context ? context->frameSerial : 0);
    const auto policy = mcvr::configuredSceneReleasePolicy();
    mcvr::log::info("WorldRelease") << "Scene released; descriptors=" << policy.descriptors
        << " reconstruction=" << policy.reconstruction << std::endl;
    dumpReleasedSceneAllocator(framework->vma());
}

void World::close() {
    shouldRenderWorld_ = false;
    chunks_->close();
    entities_->close();
    instancing_->close();
}
