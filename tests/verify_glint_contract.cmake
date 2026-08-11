if(NOT DEFINED MCVR_SOURCE_DIR)
    message(FATAL_ERROR "MCVR_SOURCE_DIR is required")
endif()

set(GLINT_MATERIAL "${MCVR_SOURCE_DIR}/src/shader/util/glint_material.glsl")
set(VERTEX_GLSL "${MCVR_SOURCE_DIR}/src/shader/util/vertex.glsl")
set(VERTEX_CPP "${MCVR_SOURCE_DIR}/src/core/vulkan/vertex.cpp")
set(SHARED_HPP "${MCVR_SOURCE_DIR}/src/common/shared.hpp")

foreach(required_file IN ITEMS
        "${GLINT_MATERIAL}" "${VERTEX_GLSL}" "${VERTEX_CPP}" "${SHARED_HPP}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Missing glint contract source: ${required_file}")
    endif()
endforeach()

file(READ "${GLINT_MATERIAL}" glint_material_source)
foreach(marker IN ITEMS
        "worldUBO.glintStrength"
        "mat.f0 = max"
        "mat.roughness = mix"
        "return glintColor")
    string(FIND "${glint_material_source}" "${marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR "Glint PBR material contract is missing: ${marker}")
    endif()
endforeach()
foreach(forbidden_marker IN ITEMS "mat.transmission" "albedoValue.a" "glintColor * 0.35")
    string(FIND "${glint_material_source}" "${forbidden_marker}" marker_index)
    if(NOT marker_index EQUAL -1)
        message(FATAL_ERROR "Glint material changes forbidden surface semantics: ${forbidden_marker}")
    endif()
endforeach()

file(READ "${VERTEX_GLSL}" vertex_glsl_source)
file(READ "${VERTEX_CPP}" vertex_cpp_source)
file(READ "${SHARED_HPP}" shared_source)
foreach(marker IN ITEMS
        "GLINT_MODE_SHIFT = 18u"
        "GLINT_MODE_ITEM = 1u"
        "ITEM_GLINT_UV_SCALE = 50.0"
        "transformGlintUv")
    string(FIND "${vertex_glsl_source}" "${marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR "Packed glint mode contract is missing: ${marker}")
    endif()
endforeach()
string(FIND "${vertex_cpp_source}" "(vertex.useGlint & 0x3u) << glintModeShift" packed_mode_index)
if(packed_mode_index EQUAL -1)
    message(FATAL_ERROR "Native vertex packing does not preserve the glint mode")
endif()
string(FIND "${shared_source}" "T_FLOAT glintStrength" glint_strength_index)
if(glint_strength_index EQUAL -1)
    message(FATAL_ERROR "WorldUBO does not carry the vanilla glint strength")
endif()

set(SURFACE_SHADERS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/default.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/no_height.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/world/default.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/world/no_height.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/surface_eval.glsl")
foreach(surface_shader IN LISTS SURFACE_SHADERS)
    file(READ "${surface_shader}" surface_source)
    string(FIND "${surface_source}" "applyGlintMaterialLayer" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR "${surface_shader} is missing the glint material layer")
    endif()
endforeach()

foreach(shadow_shader IN ITEMS
        "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/shadow.rahit"
        "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/shadow.rahit")
    file(READ "${shadow_shader}" shadow_source)
    foreach(forbidden_marker IN ITEMS "glintTexture" "glintUV" "vec3 glint")
        string(FIND "${shadow_source}" "${forbidden_marker}" marker_index)
        if(NOT marker_index EQUAL -1)
            message(FATAL_ERROR "Glint still changes shadow throughput in ${shadow_shader}: ${forbidden_marker}")
        endif()
    endforeach()
endforeach()
