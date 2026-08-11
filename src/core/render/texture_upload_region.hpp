#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mcvr {
struct TextureUploadRegion {
    size_t sourceOffset, sourceStride, rowBytes, rows, packedBytes;

    static TextureUploadRegion checked(size_t sourceBytes, uint32_t rowPixels,
                                       int x, int y, uint32_t width, uint32_t height,
                                       uint32_t bytesPerPixel) {
        if (x < 0 || y < 0 || !width || !height || !bytesPerPixel ||
            uint64_t(x) + width > rowPixels)
            throw std::invalid_argument("Invalid texture upload rectangle");
        const uint64_t stride = uint64_t(rowPixels) * bytesPerPixel;
        const uint64_t row = uint64_t(width) * bytesPerPixel;
        // Bounds expressed as division/subtraction so hostile dimensions cannot wrap.
        if (uint64_t(y) > sourceBytes / stride)
            throw std::out_of_range("Texture upload exceeds source allocation");
        const uint64_t offset = uint64_t(y) * stride + uint64_t(x) * bytesPerPixel;
        if (offset > sourceBytes || row > sourceBytes - offset ||
            uint64_t(height - 1) > (sourceBytes - offset - row) / stride)
            throw std::out_of_range("Texture upload exceeds source allocation");
        return {size_t(offset), size_t(stride), size_t(row), height, size_t(row * height)};
    }

    void copy(void *destination, const void *source) const {
        if (!destination || !source) throw std::invalid_argument("Null texture upload memory");
        auto *dst = static_cast<uint8_t *>(destination);
        auto *src = static_cast<const uint8_t *>(source) + sourceOffset;
        if (rowBytes == sourceStride) {
            std::memcpy(dst, src, packedBytes);
        } else {
            for (size_t y = 0; y < rows; ++y)
                std::memcpy(dst + y * rowBytes, src + y * sourceStride, rowBytes);
        }
    }
};
}
