#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcvr::ponder {

struct Extent { uint32_t width; uint32_t height; };

// UI path tracing is bounded independently from the physical backbuffer. The
// final composite still uses the physical target, so GUI coordinates do not
// change and only PT/history allocations are reduced.
constexpr uint64_t pixelBudget = 1920ull * 1080ull;
constexpr uint32_t maximumDimension = 1920;

inline Extent boundedExtent(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return {0, 0};
    const double byPixels = std::sqrt(static_cast<double>(pixelBudget) /
                                      (static_cast<double>(width) * height));
    const double byDimension = static_cast<double>(maximumDimension) /
                               std::max(width, height);
    const double scale = std::min({1.0, byPixels, byDimension});
    return {
        std::max(1u, static_cast<uint32_t>(std::lround(width * scale))),
        std::max(1u, static_cast<uint32_t>(std::lround(height * scale))),
    };
}

} // namespace mcvr::ponder
