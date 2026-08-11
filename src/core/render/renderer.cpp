#include "core/logging.hpp"
#include "core/render/renderer.hpp"
#include "core/diagnostics/local_gpu_capture.hpp"

#include "core/render/buffers.hpp"
#include "core/render/framebuffers.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/textures.hpp"
#include "core/render/world.hpp"
#include "core/render/scene_scope.hpp"
#include "core/loading/loading_renderer.hpp"

std::filesystem::path Renderer::folderPath{};
Options Renderer::options{};

Renderer::Renderer(GLFWwindow *window)
    : framework_(Framework::create(window)),
      textures_(Textures::create(framework_)),
      framebuffers_(Framebuffers::create(framework_, textures_)),
      buffers_(Buffers::create(framework_)),
      world_(World::create(framework_)) {}

Renderer::~Renderer() {}

std::shared_ptr<Framework> Renderer::framework() {
    return framework_;
}

std::shared_ptr<Textures> Renderer::textures() {
    return textures_;
}

std::shared_ptr<Framebuffers> Renderer::framebuffers() {
    return framebuffers_;
}

std::shared_ptr<Buffers> Renderer::buffers() {
    if (auto scene = SceneRecordingScope::active()) return scene->buffers;
    return buffers_;
}

std::shared_ptr<World> Renderer::world() {
    if (auto scene = SceneRecordingScope::active()) return scene->world;
    return world_;
}

void Renderer::close() {
    if (framework_ != nullptr && framework_->isDeviceLost())
        mcvr::diagnostics::local_gpu_capture::beforeLostDeviceRelease();
    releaseLoadingRenderer();
    if (framework_ != nullptr && !framework_->isDeviceLost()) framework_->waitDeviceIdle();

    if (world_ != nullptr) world_->close();
    if (framework_ != nullptr) framework_->close();

    framebuffers_ = nullptr;
    textures_ = nullptr;
    buffers_ = nullptr;
    world_ = nullptr;
    framework_ = nullptr;

#ifdef DEBUG
    mcvr::log::info("Renderer") << "Renderer closed" << std::endl;
#endif
}
