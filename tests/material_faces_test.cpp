#include "core/render/material_faces.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>
using namespace mcvr::faces;
void require(bool v) {
    if (!v) throw std::runtime_error("material facing contract");
}
static void checkModel(std::span<const uint32_t> model) {
    ModelRules rules(model);
    const uint32_t face = uniform(model);
    for (float scale : {-2.0f, 0.0f, 3.0f}) {
        VkTransformMatrixKHR transform{{{scale, 0, 0, 10}, {0, 1, 0, -8}, {0, 0, 1, 4}}};
        uint32_t expected = face ? 0 : VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        if ((scale < 0) != (face == front)) expected |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
        require(rules.instanceFlags(transform) == expected);
        for (auto material : model) {
            require(rules.shaderFlags(material) == (face ? material & ~mask : material));
            require(rules.needsAnyHit(material) == (effective(material) != 0 && !face));
            // Non-face material/owner bits survive both hardware and any-hit routes.
            require((rules.shaderFlags(material) & ~mask) == (material & ~mask));
        }
    }
}

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
// Reference the old per-geometry traversal, outside production. Optional benchmark only:
// no wall-clock threshold is a test oracle and these synthetic models are not game FPS.
NOINLINE static uint64_t legacyTraversal(std::span<const uint32_t> model) {
    uint64_t sum = 0;
    for (auto m : model) {
        sum += uniform(model) ? m & ~mask : m;
        sum += effective(m) != 0 && uniform(model) == 0;
    }
    return sum;
}
NOINLINE static uint64_t modelTraversal(std::span<const uint32_t> model) {
    const ModelRules rules(model);
    uint64_t sum = 0;
    for (auto m : model) sum += rules.shaderFlags(m) + rules.needsAnyHit(m);
    return sum;
}
static void benchmark() {
    std::cout << "geometries,legacy_ms,model_ms,checksum\n";
    for (size_t n : {4u, 32u, 256u, 1024u}) {
        std::vector<uint32_t> model(n, back | (1u << 10));
        auto measure = [&](auto fn) {
            uint64_t sum = 0;
            auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < 2000; ++i) {
                model.back() = back | ((i & 1u) << 10); // vary non-face data, preserve uniformity
                sum += fn(model);
            }
            return std::pair(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count(), sum);
        };
        auto old = measure(legacyTraversal), now = measure(modelTraversal);
        require(old.second == now.second);
        std::cout << n << ',' << old.first << ',' << now.first << ',' << now.second << '\n';
    }
}

int main(int argc, char **argv) {
    std::array<uint32_t, 2> mixed{back, 0}, single{back, back}, inverted{back | clockwise, front};
    require(!uniform(mixed));
    require(uniform(single) == back);
    require(uniform(inverted) == front);
    require(needsAnyHit(mixed[0], mixed));
    require(!needsAnyHit(mixed[1], mixed));
    require(!needsAnyHit(single[0], single));
    for (bool face : {false, true}) {
        require(accepts(0, face));
        require(accepts(back, face) == face);
        require(accepts(front, face) != face);
        require(!accepts(front | back, face));
        require(accepts(back | clockwise, face) != face);
    }
    require(accepts(back, false) == accepts(back | clockwise, true));
    const std::array<uint32_t, 8> modes{0, back, front, back | front,
        clockwise, back | clockwise, front | clockwise, back | front | clockwise};
    checkModel({});
    for (auto a : modes) for (auto b : modes) for (auto c : modes) {
        std::array<uint32_t, 3> model{a | (1u << 10), b | (1u << 12), c | (9u << 24)};
        checkModel(model);
        require(legacyTraversal(model) == modelTraversal(model));
    }
    // A model can change materials without changing its vector allocation. The next
    // traversal must recompute the decision, including mixed opaque/double-sided geometry.
    std::vector<uint32_t> changing(1024, back);
    checkModel(changing);
    changing.back() = 0;
    checkModel(changing);
    require(ModelRules(changing).needsAnyHit(back));
    changing.assign(1024, front);
    checkModel(changing);
    require(!ModelRules(changing).needsAnyHit(front));
    std::cout << "Model rules: 512 mixed models, mirrored transforms and material replacement passed\n";
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") benchmark();
}
