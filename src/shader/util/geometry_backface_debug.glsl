#ifndef GEOMETRY_BACKFACE_DEBUG_GLSL
#define GEOMETRY_BACKFACE_DEBUG_GLSL
#include "util/material_faces.glsl"
// Default-off isolated diagnostic. Paired-sheet ownership remains active in the
// any-hit stage, so a rejected coincident twin cannot produce a false backface.
bool geometryBackfaceDebug(inout MainRay ray, uint sourceFlags) {
#if MCVR_GEOMETRY_BACKFACES
    if (rayBounce(ray) == 0u) {
        bool front=(gl_HitKindEXT==gl_HitKindFrontFacingTriangleEXT)!=((sourceFlags & materialClockwiseBit)!=0u);
        if ((sourceFlags & (materialCullBackBit|materialCullFrontBit))==materialCullFrontBit) front=!front;
        if (!front) {
            ray.radiance=vec3(4.0,0.0,2.0);
            ray.hitT=gl_HitTEXT;
            ray.normal=-normalize(gl_WorldRayDirectionEXT);
            ray.hasPrevScenePos=0u;
            raySetStop(ray,true);
            raySetContinue(ray,false);
            return true;
        }
    }
#endif
    return false;
}
#endif
