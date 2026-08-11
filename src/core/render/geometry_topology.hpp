#pragma once
#include "core/render/quad_topology.hpp"
#include "core/render/geometry_policy.hpp"
#include "core/vulkan/vertex.hpp"

namespace mcvr::geometry {
inline constexpr uint32_t pairedGeometryBit = 1u<<19;
struct PreparedTopology {
    std::vector<uint32_t> vertexFlags;
    topology::Report report;
};
inline PreparedTopology prepare(std::vector<vk::VertexFormat::PBRVertex> &vertices,
                                std::vector<uint32_t> &indices, bool authoredQuads) {
    std::vector<topology::Point> positions;
    positions.reserve(vertices.size());
    for(const auto &v:vertices) positions.push_back({v.pos.x,v.pos.y,v.pos.z});
    auto out=topology::reconcileAuthored(positions,indices,authoredQuads,concordantQuads());
    if(out.report.nonfinite) throw std::invalid_argument("Nonfinite quad position");
    indices=std::move(out.indices);
    PreparedTopology result{{},out.report};
    if(out.report.pairedQuads || out.report.pairedTriangles) result.vertexFlags=topology::attachPrimitiveFlags(vertices,indices,out.primitiveFlags);
    return result;
}
} // namespace mcvr::geometry
