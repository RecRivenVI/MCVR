#ifndef CAMERA_RAY_GLSL
#define CAMERA_RAY_GLSL

// Build a world-space camera ray for both perspective and orthographic projections.
// All screen-space passes must use this helper so a scene cannot be rendered with
// different camera models in its primary, priority, and lighting paths.
void buildWorldCameraRay(WorldUBO camera, vec2 pixelCenter, vec2 resolution,
                         out vec3 origin, out vec3 direction) {
    vec2 ndc = pixelCenter / resolution * 2.0 - 1.0;
    vec4 viewNear = camera.cameraProjMatInv * vec4(ndc, 0.0, 1.0);
    viewNear /= viewNear.w;

    origin = vec3(camera.cameraEffectedViewMatInv * vec4(0.0, 0.0, 0.0, 1.0));
    direction = normalize(vec3(camera.cameraEffectedViewMatInv * vec4(viewNear.xyz, 0.0)));
    if (abs(camera.cameraProjMat[3][3]) > 0.5) {
        vec4 viewFar = camera.cameraProjMatInv * vec4(ndc, 1.0, 1.0);
        viewFar /= viewFar.w;
        origin = vec3(camera.cameraEffectedViewMatInv * vec4(viewNear.xyz, 1.0));
        direction = normalize(vec3(camera.cameraEffectedViewMatInv
            * vec4(viewFar.xyz - viewNear.xyz, 0.0)));
    }
}

#endif
