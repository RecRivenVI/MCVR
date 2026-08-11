#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

#include "common/shared.hpp"
#include "util/priority_payload.glsl"

layout(location = 0) rayPayloadInEXT PriorityRayPayload priorityRay;

void main() {
    priorityRay.color = vec4(0.0);
    priorityRay.hitT = INF_DISTANCE;
}
