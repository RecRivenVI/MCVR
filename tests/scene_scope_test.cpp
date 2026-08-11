#include "core/render/scene_scope.hpp"
#include <stdexcept>
#include <thread>
#include <iostream>

int main() {
    auto require = [](bool value) { if (!value) throw std::runtime_error("Scene scope isolation failed"); };
    require(SceneRecordingScope::active() == nullptr);
    SceneRecordingState mainPreview, transitionPreview;
    {
        SceneRecordingScope outer(mainPreview);
        require(SceneRecordingScope::active() == &mainPreview);
        try {
            SceneRecordingScope inner(transitionPreview);
            require(SceneRecordingScope::active() == &transitionPreview);
            throw 1;
        } catch (int) {}
        require(SceneRecordingScope::active() == &mainPreview);
        bool isolated = false;
        std::thread other([&] {
            isolated = SceneRecordingScope::active() == nullptr;
            SceneRecordingScope worker(transitionPreview);
            isolated = isolated && SceneRecordingScope::active() == &transitionPreview;
        });
        other.join();
        require(isolated);
        require(SceneRecordingScope::active() == &mainPreview);
    }
    require(SceneRecordingScope::active() == nullptr);
    std::cout << "PASS: nested, exceptional-exit and thread-local scene isolation\n";
}
