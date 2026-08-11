#include "core/render/instancing_contract.hpp"
#include "common/shared.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        static_assert(sizeof(vk::VertexFormat::InstanceAppearance) == 368);
        static_assert(offsetof(vk::VertexFormat::InstanceAppearance, lightScene) == 92);
        const auto embedding = glm::translate(glm::mat4(1), glm::vec3(10, 20, 30));
        const auto instanceLocal = glm::translate(glm::mat4(1), glm::vec3(2, 3, 4));
        const glm::ivec3 renderOrigin(512, 64, -256);
        if (glm::vec3(mcvr::instancing::instanceWorldTransform(renderOrigin, embedding, instanceLocal)[3])
            != glm::vec3(524, 87, -222))
            throw std::runtime_error("Embedded instance lost engine render origin");
        const glm::ivec3 farOrigin(29999984, 64, -29999984);
        const glm::dvec3 farCamera(29999990.25, 66.5, -29999980.75);
        const auto farRelative = mcvr::instancing::cameraRelativeTransform(
            farOrigin, embedding * instanceLocal, farCamera);
        const glm::vec3 expectedFarRelative(5.75f, 20.5f, 30.75f);
        if (glm::distance(glm::vec3(farRelative[3]), expectedFarRelative) > 1.0e-5f)
            throw std::runtime_error("Large-coordinate Flywheel transform lost camera-relative precision");
        if (glm::vec3(mcvr::instancing::instanceLightTransform(0, renderOrigin, embedding, instanceLocal)[3])
            != glm::vec3(524, 87, -222))
            throw std::runtime_error("Static lighting scene lost absolute world origin");
        if (glm::vec3(mcvr::instancing::instanceLightTransform(7, renderOrigin, embedding, instanceLocal)[3])
            != glm::vec3(12, 23, 34))
            throw std::runtime_error("Plot lighting scene incorrectly received physical world origin");
        const uint64_t packedSection = (static_cast<uint64_t>(-7) & 0x3FFFFFu) << 42 |
            (static_cast<uint64_t>(19) & 0x3FFFFFu) << 20 |
            (static_cast<uint64_t>(-3) & 0xFFFFFu);
        const auto section = mcvr::instancing::sectionCoordinate(static_cast<int64_t>(packedSection));
        if (section != glm::ivec3(-7, -3, 19))
            throw std::runtime_error("Minecraft SectionPos signed coordinate decode drifted");
        for (uint32_t textMode = 12; textMode <= 19; ++textMode) {
            if (mcvr::instancing::isFlywheelAlphaMode(textMode))
                throw std::runtime_error("Flywheel alpha mode collided with Radiance text modes");
        }
        for (uint32_t flywheelMode : {11u, 20u, 21u, 22u, 23u}) {
            if (!mcvr::instancing::isFlywheelAlphaMode(flywheelMode))
                throw std::runtime_error("Flywheel alpha mode escaped five-bit contract");
        }
        const glm::vec4 src(0.2f, 0.4f, 0.8f, 0.25f);
        const glm::vec4 dst(0.7f, 0.5f, 0.1f, 0.6f);
        auto requireBlend = [&](mcvr::instancing::FlywheelBlend mode, glm::vec4 expected) {
            if (glm::distance(mcvr::instancing::blendFlywheel(src, dst, mode), expected) > 1.0e-6f)
                throw std::runtime_error("Flywheel blend equation drifted");
        };
        requireBlend(mcvr::instancing::FlywheelBlend::Additive, src + dst);
        requireBlend(mcvr::instancing::FlywheelBlend::Lightning, src * src.a + dst);
        requireBlend(mcvr::instancing::FlywheelBlend::Glint,
            glm::vec4(glm::vec3(src) * glm::vec3(src) + glm::vec3(dst), dst.a));
        requireBlend(mcvr::instancing::FlywheelBlend::Crumbling,
            glm::vec4(2.0f * glm::vec3(src) * glm::vec3(dst), src.a));
        requireBlend(mcvr::instancing::FlywheelBlend::Translucent,
            glm::vec4(glm::vec3(src) * src.a + glm::vec3(dst) * (1.0f - src.a),
                src.a + dst.a * (1.0f - src.a)));
        if (std::abs(mcvr::instancing::chunkDiffuse(glm::vec3(0, 1, 0), false) - 1.0f) > 1.0e-6f ||
            std::abs(mcvr::instancing::chunkDiffuse(glm::vec3(0, -1, 0), false) - 0.5f) > 1.0e-6f ||
            std::abs(mcvr::instancing::chunkDiffuse(glm::vec3(0, 1, 0), true) - 0.9f) > 1.0e-6f ||
            std::abs(mcvr::instancing::chunkDiffuse(glm::vec3(1, 0, 0), true) - 0.6f) > 1.0e-6f)
            throw std::runtime_error("Flywheel chunk/nether diffuse contract drifted");
        const glm::mat4 instanceTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.25f, 0.0f, 0.75f));
        const glm::mat4 embeddingTransform = glm::translate(glm::mat4(1.0f), glm::vec3(17.3f, -2.0f, 5.6f)) *
            glm::rotate(glm::mat4(1.0f), 0.8f, glm::vec3(0, 1, 0));
        const glm::vec3 localVertex(0.4f, 0.2f, 0.7f);
        const glm::vec3 expectedCrumbling = glm::vec3(instanceTransform * glm::vec4(localVertex, 1.0f));
        if (glm::distance(mcvr::instancing::crumblingPosition(instanceTransform, localVertex), expectedCrumbling) > 1.0e-6f ||
            glm::distance(expectedCrumbling,
                glm::vec3(embeddingTransform * instanceTransform * glm::vec4(localVertex, 1.0f))) < 1.0f)
            throw std::runtime_error("Crumbling UV position incorrectly includes embedding transform");
        const glm::mat4 pose = glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0, 1, 0)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 0.5f, 3.0f));
        const glm::mat3 explicitNormal = glm::mat3(glm::rotate(glm::mat4(1.0f), -0.3f, glm::vec3(1, 0, 0))) *
            glm::mat3(1.3f);
        const auto correction = mcvr::instancing::normalCorrection(pose, explicitNormal);
        const auto rayNormal = glm::transpose(glm::inverse(glm::mat3(pose)));
        for (glm::vec3 normal : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(1, 2, 3)}) {
            if (glm::length(rayNormal * correction * normal - explicitNormal * normal) > 1.0e-5f)
                throw std::runtime_error("Explicit Flywheel normal lost after TLAS inverse-transpose");
        }
        constexpr int sizes[]{76, 112, 52, 36, 52, 72, 108, 88};
        for (int adapter = 0; adapter < 8; ++adapter) {
            if (!mcvr::instancing::validInstanceBytes(adapter, sizes[adapter]) ||
                mcvr::instancing::validInstanceBytes(adapter, sizes[adapter] - 1) ||
                mcvr::instancing::validInstanceBytes(adapter, sizes[adapter] + 1))
                throw std::runtime_error("Flywheel ABI accepted a truncated or mismatched record");
        }
        if (mcvr::instancing::validInstanceBytes(-1, 76) || mcvr::instancing::validInstanceBytes(8, 76))
            throw std::runtime_error("Unknown Flywheel adapter accepted");

        // Animation clock precision. Angles compare modulo a full turn.
        auto angleError = [](double a, double b) {
            const double d = std::fmod(std::abs(a - b), 360.0);
            return std::min(d, 360.0 - d);
        };
        auto exactDegrees = [](double offset, double speed, double ticks) { return offset + ticks / 20.0 * speed; };
        auto formerDegrees = [](float offset, float speed, double ticks) {
            return offset + static_cast<float>(ticks / 20.0 * speed); // previous float product
        };
        // Small session times: same rotation as before.
        if (angleError(mcvr::instancing::rotationDegrees(10.0f, 96.0f, 123.4), formerDegrees(10.0f, 96.0f, 123.4)) > 1.0e-3)
            throw std::runtime_error("Rotation changed for small render times");
        // A 7,680,653-tick world clock (observed in a user world): the float product was quantized to
        // several degrees; the reduced double evaluation stays exact and advances with partial ticks.
        const double largeTicks = 7680653.0;
        for (float speed : {96.0f, 1536.0f, -48.0f}) {
            for (double partial : {0.0, 0.25, 0.5, 0.75}) {
                const double ticks = largeTicks + partial;
                if (angleError(mcvr::instancing::rotationDegrees(3.0f, speed, ticks), exactDegrees(3.0, speed, ticks)) > 1.0e-3)
                    throw std::runtime_error("Large-clock rotation lost precision");
            }
        }
        // 256 RPM: the former float product is off by 6.4 degrees here (64-degree float spacing).
        if (angleError(formerDegrees(0.0f, 1536.0f, largeTicks + 0.25), exactDegrees(0.0, 1536.0, largeTicks + 0.25)) < 5.0)
            throw std::runtime_error("Test no longer reproduces the former float quantization");
        const glm::vec2 speed(0.0625f, -0.03125f), offset(0.2f, 0.7f);
        auto exactPhase = [&](double ticks, float s, float o) {
            const double v = double(s) * ticks + o;
            return v - std::floor(v);
        };
        for (double ticks : {12.25, largeTicks + 0.25, largeTicks + 0.75}) {
            const auto phase = mcvr::instancing::scrollPhase(speed, ticks, offset);
            if (std::abs(phase.x - exactPhase(ticks, speed.x, offset.x)) > 1.0e-6 ||
                std::abs(phase.y - exactPhase(ticks, speed.y, offset.y)) > 1.0e-6 || phase.x < 0 || phase.x >= 1)
                throw std::runtime_error("Scroll phase lost precision or range");
        }
        if (static_cast<float>(largeTicks + 0.25) != static_cast<float>(largeTicks))
            throw std::runtime_error("Test no longer reproduces the former float partial-tick loss");
        std::cout << "PASS: explicit normals survive nonuniform TLAS transforms; exact instance ABI sizes\n";
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
