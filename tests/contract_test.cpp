#include "com_radiance_client_option_Options.h"
#include "core/render/index_patterns.hpp"

#include <cstdint>
#include <vector>

#ifndef _Included_com_radiance_client_option_Options
#error "The generated Radiance JNI contract header was not included"
#endif

int main() {
    if (MCVR_EXPECTED_JNI_HEADER_COUNT <= 0) {
        return 1;
    }
    using mcvr::render::buildFourVertexIndices;
    using mcvr::render::FourVertexIndexPattern;
    if (buildFourVertexIndices<uint16_t>(4, 6, FourVertexIndexPattern::MINECRAFT_LINES) !=
        std::vector<uint16_t>{0, 1, 2, 3, 2, 1}) {
        return 2;
    }
    if (buildFourVertexIndices<uint16_t>(4, 6, FourVertexIndexPattern::QUADS) !=
        std::vector<uint16_t>{0, 1, 2, 2, 3, 0}) {
        return 3;
    }
    if (buildFourVertexIndices<uint32_t>(8, 12, FourVertexIndexPattern::MINECRAFT_LINES) !=
        std::vector<uint32_t>{0, 1, 2, 3, 2, 1, 4, 5, 6, 7, 6, 5}) {
        return 4;
    }
    return 0;
}
