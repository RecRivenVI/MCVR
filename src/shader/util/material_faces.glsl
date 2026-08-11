#ifndef MATERIAL_FACES_GLSL
#define MATERIAL_FACES_GLSL
// gl_HitKind already includes the TLAS mirror correction, never a shading normal.
bool acceptsMaterialFace(uint flags, bool ccwFront) {
    bool front = ccwFront != ((flags & (1u << 22u)) != 0u);
    return (flags & (front ? (1u << 7u) : (1u << 2u))) == 0u;
}
#endif
