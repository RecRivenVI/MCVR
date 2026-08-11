#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

#include "util/priority_payload.glsl"

#ifndef PRIORITY_OUTLINE_COLOR
#define PRIORITY_OUTLINE_COLOR vec3(1.0)
#endif

layout(location = 0) rayPayloadInEXT PriorityRayPayload priorityRay;
hitAttributeEXT vec2 attribs;

void main() {
    priorityRay.color = vec4(PRIORITY_OUTLINE_COLOR, 1.0);
    priorityRay.hitT = gl_HitTEXT;
}
