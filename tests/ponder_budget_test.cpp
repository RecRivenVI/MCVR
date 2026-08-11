#include "core/render/ponder_budget.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        const auto small = mcvr::ponder::boundedExtent(1280, 720);
        if (small.width != 1280 || small.height != 720)
            throw std::runtime_error("small UI viewport was enlarged or changed");
        const auto large = mcvr::ponder::boundedExtent(3840, 2054);
        if (large.width > mcvr::ponder::maximumDimension ||
            large.height > mcvr::ponder::maximumDimension ||
            uint64_t(large.width) * large.height > mcvr::ponder::pixelBudget)
            throw std::runtime_error("large UI viewport exceeds its PT budget");
        const double inputAspect = 3840.0 / 2054.0;
        const double outputAspect = double(large.width) / large.height;
        if (std::abs(inputAspect - outputAspect) > 0.002)
            throw std::runtime_error("bounded UI viewport changed aspect ratio");
        std::cout << "Ponder/UI PT extent budget passed: " << large.width << 'x' << large.height << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
