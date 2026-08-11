#pragma once
#include <glm/glm.hpp>
#include <cmath>
#include <stdexcept>

// Factor an affine GUI camera into a proper rigid view and a projection that
// retains all GUI scale/reflection. Preserve camera center and the viewing axis.
inline void normalizeSceneCamera(glm::mat4 &view, glm::mat4 &projection) {
    const glm::dmat4 original(view);
    const glm::dmat3 linear(original);
    const double determinant = glm::determinant(linear);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
        throw std::runtime_error("Degenerate Ponder camera");
    glm::dvec3 z(original[0][2], original[1][2], original[2][2]);
    z = glm::normalize(z);
    glm::dvec3 x(original[0][0], original[1][0], original[2][0]);
    x = glm::normalize(x - z * glm::dot(x, z));
    const glm::dvec3 y = glm::cross(z, x);
    const glm::dmat3 rotation = glm::transpose(glm::dmat3(x, y, z));
    const glm::dvec3 center = -glm::inverse(linear) * glm::dvec3(original[3]);
    glm::dmat4 rigid(rotation);
    rigid[3] = glm::dvec4(-rotation * center, 1.0);
    projection = glm::mat4(glm::dmat4(projection) * original * glm::inverse(rigid));
    view = glm::mat4(rigid);
}
