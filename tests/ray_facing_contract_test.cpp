#include "core/render/material_faces.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

void require(bool value) { if (!value) throw std::runtime_error("ray-facing contract"); }
int main() {
    using namespace mcvr::faces;
    constexpr std::array roles{QueryRole::sourceSelection, QueryRole::lightVisibility, QueryRole::radianceEndpoint};
    unsigned checks = 0;
    for (uint32_t flags : {0u, back, front, back | front, clockwise, back | clockwise, front | clockwise}) {
        for (bool queryFront : {false, true}) {
            for (auto role : roles) {
                require(acceptsTraversalHit(flags, queryFront, role) == accepts(flags, queryFront));
                ++checks;
            }
            const uint32_t optimized = flags | hardwareFrontFlip;
            for (auto role : roles) {
                require(acceptsTraversalHit(optimized, !queryFront, role) == accepts(flags, queryFront));
                ++checks;
            }
        }
    }
    require(accepts(back, true) && !accepts(back, false));
    require(!accepts(front, true) && accepts(front, false));
    // Same BLAS with unlike policies: each geometry retains its own answer.
    const std::array mixed{back, 0u};
    const ModelRules model(mixed, false);
    require(model.needsAnyHit(back));
    for (bool side : {false, true}) {
        require(acceptsTraversalHit(model.shaderFlags(back), side, QueryRole::radianceEndpoint) == side);
        require(acceptsTraversalHit(model.shaderFlags(0), side, QueryRole::radianceEndpoint));
    }
    std::cout << checks << " ray-role/provenance checks: accepted surface owns material response; rejected side has no hit\n";
}
