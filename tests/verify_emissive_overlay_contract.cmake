if(NOT DEFINED MCVR_SOURCE_DIR)
    message(FATAL_ERROR "MCVR_SOURCE_DIR is required")
endif()

function(require_source_marker file marker description)
    file(READ "${file}" source)
    string(FIND "${source}" "${marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR "Missing ${description} in ${file}")
    endif()
endfunction()

function(reject_source_marker file marker description)
    file(READ "${file}" source)
    string(FIND "${source}" "${marker}" marker_index)
    if(NOT marker_index EQUAL -1)
        message(FATAL_ERROR "Unexpected ${description} in ${file}")
    endif()
endfunction()

set(SHARED_HEADER "${MCVR_SOURCE_DIR}/src/common/shared.hpp")
set(ENTITIES_SOURCE "${MCVR_SOURCE_DIR}/src/core/render/entities.cpp")
set(VERTEX_SOURCE "${MCVR_SOURCE_DIR}/src/core/vulkan/vertex.cpp")
set(OVERLAY_HELPER "${MCVR_SOURCE_DIR}/src/shader/util/emissive_overlay.glsl")

require_source_marker("${SHARED_HEADER}" "T_UINT emissiveOverlayTextureID;"
                      "packed emissive-overlay texture field")
require_source_marker("${ENTITIES_SOURCE}" "composeEmissiveEyeOverlays("
                      "entity eyes-layer composition pass")
require_source_marker("${ENTITIES_SOURCE}" "sameTexturedSurface("
                      "position/UV/topology pairing guard")
require_source_marker("${ENTITIES_SOURCE}" "geometryGroupNames[overlayIndex] != \"eyes\""
                      "eyes RenderType restriction")
require_source_marker("${ENTITIES_SOURCE}" "vertices.erase(vertices.begin() + overlayIndex);"
                      "independent overlay geometry removal")
require_source_marker("${VERTEX_SOURCE}" ".emissiveOverlayTextureID = 0"
                      "default packed material initialization")
require_source_marker("${OVERLAY_HELPER}" "overlay.rgb"
                      "emissive-overlay color sampling")
require_source_marker("${OVERLAY_HELPER}" "overlay.a"
                      "emissive-overlay coverage sampling")

set(DIRECT_SHADED_HIT_SHADERS
    "src/shader/world/ray_tracing/internal/vanilla-pt/world/default.rchit"
    "src/shader/world/ray_tracing/internal/vanilla-pt/world/no_height.rchit"
    "src/shader/world/ray_tracing/internal/vanilla-pt/world/world_no_reflect.rchit"
    "src/shader/world/ray_tracing/internal/advanced/world/default.rchit"
    "src/shader/world/ray_tracing/internal/advanced/world/no_height.rchit"
    "src/shader/world/ray_tracing/internal/advanced/common/world_no_reflect.rchit")
foreach(shader IN LISTS DIRECT_SHADED_HIT_SHADERS)
    require_source_marker("${MCVR_SOURCE_DIR}/${shader}" "sampleEmissiveOverlay(m0.emissiveOverlayTextureID"
                          "emissive-overlay contribution")
endforeach()

set(NO_REFLECT_HIT_SHADERS
    "src/shader/world/ray_tracing/internal/vanilla-pt/world/world_no_reflect.rchit"
    "src/shader/world/ray_tracing/internal/advanced/common/world_no_reflect.rchit")
foreach(shader IN LISTS NO_REFLECT_HIT_SHADERS)
    require_source_marker("${MCVR_SOURCE_DIR}/${shader}" "float lod = 0.0;"
                          "function-scope emissive-overlay LOD fallback")
    require_source_marker("${MCVR_SOURCE_DIR}/${shader}" "lod = lodWithCone("
                          "textured emissive-overlay LOD assignment")
endforeach()

set(PRIMARY_CACHE_HIT_SHADERS
    "src/shader/world/ray_tracing/internal/advanced/primary/default.rchit"
    "src/shader/world/ray_tracing/internal/advanced/primary/no_height.rchit")
foreach(shader IN LISTS PRIMARY_CACHE_HIT_SHADERS)
    require_source_marker("${MCVR_SOURCE_DIR}/${shader}" "encodeFirstHitUint(m0.emissiveOverlayTextureID)"
                          "emissive-overlay texture cache encoding")
endforeach()

set(ADVANCED_SURFACE_SOURCE
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/direct_light_surface.glsl")
set(ADVANCED_WORLD_SOURCE
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/world/world.rgen")
require_source_marker("${ADVANCED_SURFACE_SOURCE}"
                      "sampleEmissiveOverlay(cache.emissiveOverlayTextureID"
                      "advanced emissive-overlay cache sampling")
require_source_marker("${ADVANCED_WORLD_SOURCE}" "surface.emissiveOverlayRadiance"
                      "advanced emissive-overlay path contribution")

set(VANILLA_CONFIG
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/configs.json")
set(ADVANCED_CONFIG
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/configs.json")
reject_source_marker("${VANILLA_CONFIG}" "\"eyes\":"
                     "dedicated transparent-only eyes hit group")
reject_source_marker("${ADVANCED_CONFIG}" "\"eyes\":"
                     "dedicated transparent-only eyes hit group")

set(FALLBACK_ANY_HIT_SHADERS
    "src/shader/world/ray_tracing/internal/vanilla-pt/world/transparent_only.rahit"
    "src/shader/world/ray_tracing/internal/advanced/common/transparent_only.rahit")
foreach(shader IN LISTS FALLBACK_ANY_HIT_SHADERS)
    require_source_marker("${MCVR_SOURCE_DIR}/${shader}" "if (alpha <= 0.0) { ignoreIntersectionEXT; }"
                          "unmatched transparent overlay alpha rejection")
endforeach()
