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
    T_UINT emissiveOverlay, quadIndices, pad0, pad1;
};
#ifdef __cplusplus
static_assert(sizeof(EntityConvertJob) == 64);
}
#endif
#endif
