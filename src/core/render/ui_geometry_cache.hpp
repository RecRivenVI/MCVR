#pragma once
#include "common/shared.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace mcvr::ui {
inline bool equivalentPositions(std::span<const vk::VertexFormat::PBRVertex> a,
                                std::span<const vk::VertexFormat::PBRVertex> b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) for (int j = 0; j < 3; ++j) {
        float tolerance = 1e-5f * std::max({1.0f, std::abs(a[i].pos[j]), std::abs(b[i].pos[j])});
        if (!std::isfinite(a[i].pos[j]) || !std::isfinite(b[i].pos[j]) ||
            std::abs(a[i].pos[j] - b[i].pos[j]) > tolerance) return false;
    }
    return true;
}
// GUI vertices undergo a float pose/inverse-pose round trip. Compare that limited
// rounding noise, not hashes of its unstable low bits; all material bytes stay exact.
inline bool equivalentGeometry(std::span<const vk::VertexFormat::PBRVertex> a,
                               std::span<const vk::VertexFormat::PBRVertex> b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        auto left = a[i], right = b[i];
        for (int j = 0; j < 3; ++j) {
            const float tolerance = 1e-5f * std::max({1.0f, std::abs(left.pos[j]), std::abs(right.pos[j])});
            if (!std::isfinite(left.pos[j]) || !std::isfinite(right.pos[j]) ||
                std::abs(left.pos[j]-right.pos[j]) > tolerance ||
                !std::isfinite(left.norm[j]) || !std::isfinite(right.norm[j]) ||
                std::abs(left.norm[j]-right.norm[j]) > 1e-5f) return false;
        }
        left.pos = right.pos = glm::vec3(0); left.norm = right.norm = glm::vec3(0);
        if (std::memcmp(&left, &right, sizeof(left)) != 0) return false;
    }
    return true;
}
}
