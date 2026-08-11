#include "core/render/scene_camera.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

int main() {
    try {
        for (const auto scale : {glm::vec3(30, -30, 30), glm::vec3(21, -17, 35), glm::vec3(1)}) {
            auto view = glm::translate(glm::mat4(1), glm::vec3(300, 250, -12000)) *
                glm::scale(glm::mat4(1), scale) *
                glm::rotate(glm::mat4(1), 0.61f, glm::normalize(glm::vec3(1, 2, 0.5)));
            auto projection = glm::ortho(0.f, 800.f, 600.f, 0.f, 100.f, 20000.f);
            const auto oldView = view, oldProjection = projection;
            normalizeSceneCamera(view, projection);
            const auto basis = glm::mat3(view);
            if (std::abs(glm::determinant(basis) - 1.f) > 1e-5f)
                throw std::runtime_error("View is not a proper rotation");
            const auto orthogonality = glm::transpose(basis) * basis;
            for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r)
                if (std::abs(orthogonality[c][r] - (c == r ? 1.f : 0.f)) > 1e-5f)
                    throw std::runtime_error("View retains scale or shear");
            if (glm::length(glm::inverse(view)[3] - glm::inverse(oldView)[3]) > .003f)
                throw std::runtime_error("Camera center changed");
            for (int i = -10; i <= 10; ++i) {
                const glm::vec4 point(i, i * .37f, i * -.91f, 1);
                if (glm::length(projection * view * point - oldProjection * oldView * point) > 1e-5f)
                    throw std::runtime_error("Clip position changed");
            }
            // Identical unprojected near/far points preserve orthographic rays.
            for (float depth : {-1.f, 1.f}) {
                const glm::vec4 clip(.3f, -.7f, depth, 1);
                auto a = glm::inverse(projection * view) * clip;
                auto b = glm::inverse(oldProjection * oldView) * clip;
                if (glm::length(a / a.w - b / b.w) > .01f)
                    throw std::runtime_error("Unprojected ray changed");
            }
        }
        std::cout << "Rigid view, clip positions, camera center and rays preserved\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
