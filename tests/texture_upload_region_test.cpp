#include "core/render/texture_upload_region.hpp"
#include <array>
#include <iostream>
#include <vector>

int main() {
    try {
        for (uint32_t bpp : {1u, 2u, 3u, 4u, 8u}) {
            std::vector<uint8_t> source(11 * 37 * bpp), output(3 * 2 * bpp + 16, 0xCD);
            for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t(i % 251);
            auto region = mcvr::TextureUploadRegion::checked(source.size(), 11, 4, 29, 3, 2, bpp);
            region.copy(output.data() + 8, source.data());
            if (region.packedBytes != 3 * 2 * bpp) return 1;
            for (size_t y = 0; y < 2; ++y) for (size_t x = 0; x < 3 * bpp; ++x)
                if (output[8 + y * 3 * bpp + x] != source[((29 + y) * 11 + 4) * bpp + x]) return 2;
            for (size_t i = 0; i < 8; ++i)
                if (output[i] != 0xCD || output[output.size() - i - 1] != 0xCD) return 3;
            auto whole = mcvr::TextureUploadRegion::checked(source.size(), 11, 0, 0, 11, 37, bpp);
            std::vector<uint8_t> copy(source.size()); whole.copy(copy.data(), source.data());
            if (copy != source) return 4;
        }
        for (auto offsets : {std::array{9, 0}, std::array{-1, 0}, std::array{0, 40}}) {
            bool rejected = false;
            try { mcvr::TextureUploadRegion::checked(11 * 37 * 4, 11, offsets[0], offsets[1], 3, 2, 4); }
            catch (const std::exception &) { rejected = true; }
            if (!rejected) return 5;
        }
        bool rejected = false;
        try { mcvr::TextureUploadRegion::checked(64, UINT32_MAX, INT32_MAX, INT32_MAX, 1, UINT32_MAX, 16); }
        catch (const std::exception &) { rejected = true; }
        if (!rejected) return 6;
        std::cout << "Packed texture rectangles, pitches, offsets, guards and bounds passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 10; }
}
