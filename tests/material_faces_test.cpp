#include "core/render/material_faces.hpp"
#include <array>
#include <stdexcept>
using namespace mcvr::faces;
void require(bool v) {
    if (!v) throw std::runtime_error("material facing contract");
}
int main() {
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
}
