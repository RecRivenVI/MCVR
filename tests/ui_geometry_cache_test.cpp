#include "core/render/ui_geometry_cache.hpp"
#include <array>
#include <stdexcept>
#include <iostream>
void require(bool condition) { if (!condition) throw std::runtime_error("UI geometry cache regression"); }
int main() {
    std::array<vk::VertexFormat::PBRVertex, 3> before{};
    before[0].pos = {1, 2, 3}; before[0].norm = {0, 1, 0};
    auto after = before;
    after[0].pos.x += 0.000001f;
    require(mcvr::ui::equivalentGeometry(before, after));
    after[0].pos.x += .001f;
    require(!mcvr::ui::equivalentGeometry(before, after));
    after = before; after[0].textureID = 3;
    require(mcvr::ui::equivalentPositions(before, after));
    require(!mcvr::ui::equivalentGeometry(before, after));
    after = before; after[0].textureUV.x = .001f;
    require(!mcvr::ui::equivalentGeometry(before, after));
    after = before; after[0].norm.x = .1f;
    require(mcvr::ui::equivalentPositions(before, after));
    require(!mcvr::ui::equivalentGeometry(before, after));
    require(!mcvr::ui::equivalentGeometry(before, std::span(after).first(2)));
    after[0].pos.z += 0.1f;
    require(!mcvr::ui::equivalentPositions(before, after));
    std::cout << "pose rounding reuse; geometry, material and normal invalidation passed\n";
}
