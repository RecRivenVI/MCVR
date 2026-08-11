set(CAMERA_HELPER "${MCVR_SOURCE_DIR}/src/shader/util/camera_ray.glsl")
file(READ "${CAMERA_HELPER}" CAMERA_SOURCE)
foreach(REQUIRED IN ITEMS
        "camera.cameraProjMatInv"
        "abs(camera.cameraProjMat[3][3]) > 0.5"
        "viewFar.xyz - viewNear.xyz")
    string(FIND "${CAMERA_SOURCE}" "${REQUIRED}" FOUND)
    if(FOUND EQUAL -1)
        message(FATAL_ERROR "Shared camera ray helper is missing: ${REQUIRED}")
    endif()
endforeach()

set(CAMERA_CONSUMERS
    "src/shader/world/ray_tracing/internal/advanced/primary/primary.rgen"
    "src/shader/world/ray_tracing/internal/advanced/world/world.rgen"
    "src/shader/world/ray_tracing/internal/advanced/volumetric_light/volumetric_light.rgen"
    "src/shader/world/ray_tracing/internal/advanced/common/direct_light_surface.glsl"
    "src/shader/world/ray_tracing/internal/vanilla-pt/world/world.rgen"
    "src/shader/world/ray_tracing/internal/vanilla-pt/priority/priority.rgen"
    "src/shader/world/ray_tracing/internal/vanilla-pt/priority/background.rgen")
foreach(RELATIVE_PATH IN LISTS CAMERA_CONSUMERS)
    file(READ "${MCVR_SOURCE_DIR}/${RELATIVE_PATH}" SOURCE)
    string(FIND "${SOURCE}" "buildWorldCameraRay(" CALL_FOUND)
    if(CALL_FOUND EQUAL -1)
        message(FATAL_ERROR "Camera-producing shader bypasses the shared ray helper: ${RELATIVE_PATH}")
    endif()
    string(FIND "${SOURCE}" "cameraProjMatInv *" LOCAL_PROJECTION_FOUND)
    if(NOT LOCAL_PROJECTION_FOUND EQUAL -1)
        message(FATAL_ERROR "Camera-producing shader rebuilt projection rays locally: ${RELATIVE_PATH}")
    endif()
endforeach()
