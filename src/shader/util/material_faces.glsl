#ifndef MATERIAL_FACES_GLSL
#define MATERIAL_FACES_GLSL
const uint materialCullBackBit = 1u << 2u;
const uint materialCullFrontBit = 1u << 7u;
const uint materialClockwiseBit = 1u << 22u;
const uint materialHardwareFrontFlipBit = 1u << 23u;
uint effectiveMaterialFaces(uint flags) {
    uint faces = flags & (materialCullBackBit | materialCullFrontBit);
    if ((flags & materialClockwiseBit) == 0u) return faces;
    return ((faces & materialCullBackBit) != 0u ? materialCullFrontBit : 0u)
        | ((faces & materialCullFrontBit) != 0u ? materialCullBackBit : 0u);
}
// gl_HitKind includes mirror correction and, for uniform FRONT models, the
// backend's canonical-facing flip. Preserve source sidedness instead of deleting it.
bool acceptsMaterialFace(uint flags, bool ccwFront) {
    bool sourceFront = ccwFront != ((flags & materialHardwareFrontFlipBit) != 0u);
    bool front = sourceFront != ((flags & materialClockwiseBit) != 0u);
    return (flags & (front ? materialCullFrontBit : materialCullBackBit)) == 0u;
}
// Authored thin-sheet ownership is separate from the world sidedness policy.
// Unpaired geometry is bilateral; exact reverse twins retain one member per side.
bool acceptsWorldTriangleFace(uint flags, bool ccwFront, uint primitivePolicy, bool bilateral) {
    if (!bilateral) return acceptsMaterialFace(flags, ccwFront);
    if ((primitivePolicy & 1u) == 0u) return true;
    uint face = effectiveMaterialFaces(flags);
    if (face == 0u || face == (materialCullBackBit | materialCullFrontBit))
        flags = (flags & ~(materialCullBackBit | materialCullFrontBit)) | materialCullBackBit;
    return acceptsMaterialFace(flags, ccwFront);
}
// Camera, reflection, indirect and light-visibility rays all apply this source
// predicate to their own direction. Rejected intersections are absent; accepted
// intersections retain the material's original alpha/transmission behavior.
#endif
