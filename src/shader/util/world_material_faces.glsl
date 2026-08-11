#ifndef WORLD_MATERIAL_FACES_GLSL
#define WORLD_MATERIAL_FACES_GLSL
#include "util/material_faces.glsl"
#ifndef MCVR_WORLD_TWO_SIDED
#define MCVR_WORLD_TWO_SIDED 0
#endif
#ifndef MCVR_GEOMETRY_BACKFACES
#define MCVR_GEOMETRY_BACKFACES 0
#endif
#if (MCVR_WORLD_TWO_SIDED || MCVR_GEOMETRY_BACKFACES) && !defined(VERTEX_GLSL)
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference2 : require
#include "common/shared.hpp"
layout(set=1,binding=2) readonly buffer PolicyIndexAddresses { uint64_t addrs[]; } policyIndexAddresses;
layout(set=1,binding=4) readonly buffer PolicyPositionAddresses { uint64_t addrs[]; } policyPositionAddresses;
layout(std430,buffer_reference,buffer_reference_align=8) readonly buffer PolicyIndices { uint values[]; };
layout(std430,buffer_reference,buffer_reference_align=8) readonly buffer PolicyPositions { PositionVertex values[]; };
#endif
// The source rule remains available. Paired sheet ownership uses the original
// visible side, or authored front winding when the source draw was NO_CULL.
bool acceptsWorldMaterialFace(uint flags, bool queryFront, uint geometryIndex, uint primitive, bool primary) {
#if MCVR_WORLD_TWO_SIDED || MCVR_GEOMETRY_BACKFACES
    if (MCVR_WORLD_TWO_SIDED == 0 && !primary) return acceptsMaterialFace(flags, queryFront);
    if ((flags & (1u<<19u)) == 0u) return true;
#ifdef VERTEX_GLSL
    uint i0,i1,i2;
    loadTriangleIndices(geometryIndex,primitive,i0,i1,i2);
    PositionBuffer positions=PositionBuffer(positionBufferAddrs.addrs[geometryIndex]);
    uint primitivePolicy = positions.vertices[i0].pad0;
#else
    PolicyIndices indices=PolicyIndices(policyIndexAddresses.addrs[geometryIndex]);
    PolicyPositions positions=PolicyPositions(policyPositionAddresses.addrs[geometryIndex]);
    uint primitivePolicy = positions.values[indices.values[primitive*3u]].pad0;
#endif
    return acceptsWorldTriangleFace(flags, queryFront, primitivePolicy, true);
#else
    return acceptsMaterialFace(flags,queryFront);
#endif
}
bool acceptsWorldMaterialFace(uint flags, bool queryFront, uint geometryIndex, uint primitive) {
    return acceptsWorldMaterialFace(flags, queryFront, geometryIndex, primitive, false);
}
#endif
