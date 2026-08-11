#ifndef ENTITY_CONVERT_HPP
#define ENTITY_CONVERT_HPP
#include "mapping.hpp"
#ifdef __cplusplus
namespace mcvr {
#endif
// One tile covers up to 64 vertices/indices of a geometry. All offsets are elements,
// except source/indexSource which are 32-bit words in the immutable input buffer.
struct EntityConvertJob {
    T_UINT source, indexSource, vertexOffset, indexOffset;
    T_UINT vertexCount, indexCount, first, deferred;
    T_UINT emissionPolicy, coordinate, normalOffset, emissionBits;
    // Source version 0 retains canonical 128-byte input; version 1 is compact PBR100.
    T_UINT emissiveOverlay, quadIndices, sourceWords, sourceVersion;
    // Optional leading-vertex primitive tags; absent when hasPositionFlags==0.
    T_UINT positionFlagsSource, hasPositionFlags, reserved0, reserved1;
};
#ifdef __cplusplus
static_assert(sizeof(EntityConvertJob) == 80);
}
#endif
#endif
