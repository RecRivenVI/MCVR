#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <glm/gtc/packing.hpp>

namespace mcvr::readback {
struct PixelEncoding { int channels; int bytes; bool floating; bool half; bool signedNormalized; };
inline void convert(const uint8_t *pixels, int width, int height, size_t sourceStride,
                    PixelEncoding encoding, int format, int type, int outputChannels, void *destination) {
    const bool stencil = format == 0x1901;
    for (int row = 0; row < height; ++row) for (int column = 0; column < width; ++column) {
        const auto *pixel = pixels + (static_cast<size_t>(height - 1 - row) * width + column) * sourceStride;
        std::array<float, 4> values{0,0,0,1};
        for (int channel = 0; channel < encoding.channels; ++channel) {
            if (encoding.half) {
                uint16_t bits; std::memcpy(&bits, pixel + channel * 2, 2); values[channel] = glm::unpackHalf1x16(bits);
            } else if (encoding.floating) std::memcpy(&values[channel], pixel + channel * 4, 4);
            else if (encoding.bytes == 2) { uint16_t value; std::memcpy(&value, pixel + channel * 2, 2); values[channel] = value / 65535.0f; }
            else if (encoding.signedNormalized) values[channel] = std::max(-1.0f, static_cast<int8_t>(pixel[channel]) / 127.0f);
            else values[channel] = stencil ? static_cast<float>(pixel[channel]) : pixel[channel] / 255.0f;
        }
        for (int channel = 0; channel < outputChannels; ++channel) {
            const float value = values[format == 0x1906 ? 3 : channel];
            const size_t index = (static_cast<size_t>(row) * width + column) * outputChannels + channel;
            if (type == 0x1406) std::memcpy(static_cast<uint8_t *>(destination) + index * 4, &value, 4);
            else static_cast<uint8_t *>(destination)[index] = static_cast<uint8_t>(std::lround(std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, stencil ? 255.0f : 1.0f) * (stencil ? 1.0f : 255.0f)));
        }
    }
}
}
