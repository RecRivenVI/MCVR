#include "core/render/framebuffer_readback_contract.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        const uint8_t image[]{255, 0, 0, 64, 0, 255, 0, 255};
        uint8_t rgba[8]{};
        mcvr::readback::convert(image, 1, 2, 4, {4,1,false,false,false}, 0x1908, 0x1401, 4, rgba);
        const uint8_t expected[]{0,255,0,255,255,0,0,64};
        if (std::memcmp(rgba, expected, sizeof(rgba)) != 0) throw std::runtime_error("GL lower-left row order failed");
        uint8_t alpha[2]{};
        mcvr::readback::convert(image, 1, 2, 4, {4,1,false,false,false}, 0x1906, 0x1401, 1, alpha);
        if (alpha[0] != 255 || alpha[1] != 64) throw std::runtime_error("Alpha occupancy channel failed");
        const uint16_t depths[]{0,32768,65535};
        uint8_t depthBytes[3]{};
        mcvr::readback::convert(reinterpret_cast<const uint8_t *>(depths), 3, 1, 2,
            {1,2,false,false,false}, 0x1902, 0x1401, 1, depthBytes);
        if (depthBytes[0] != 0 || depthBytes[1] != 128 || depthBytes[2] != 255)
            throw std::runtime_error("D16 normalized depth conversion failed");
        const uint16_t half[]{0x3800,0xbc00};
        float floats[2]{};
        mcvr::readback::convert(reinterpret_cast<const uint8_t *>(half), 2, 1, 2,
            {1,2,true,true,false}, 0x1903, 0x1406, 1, floats);
        if (floats[0] != 0.5f || floats[1] != -1.0f) throw std::runtime_error("Floating attachment values clamped or misdecoded");
        std::cout << "PASS: readback row order, alpha, D16 normalization and half-float preservation\n";
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
